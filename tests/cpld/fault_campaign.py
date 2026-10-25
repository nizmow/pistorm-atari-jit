#!/usr/bin/env python3
"""Deterministic full-engine fault/phase campaign; every failure remains a failure."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--runner', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--latch-model', choices=('16373', '16374'), default='16374')
args = parser.parse_args()
args.runner = args.runner.resolve()
args.output.mkdir(parents=True, exist_ok=True)
cases = []

def case(name, scenario, **options):
    cases.append((name, scenario, options))

# Requests at every transfer state and at both tested initial clock phases.
for phase in (0, 43):
    for state in range(8):
        case(f'request-s{state}-p{phase}', 'fault-states', target_state=state, phase_ns=phase)
# S4 starts on a rising edge. Offsets deliberately reach all ten edge/phase bins.
for offset in (0, 60, 61, 62, 63, 64, 123, 124, 125, 126, 127):
    case(f'request-phase-{offset}', 'fault-states', target_state=4, offset_ns=offset,
         gpio_ns=31, phase_ns=17)
    case(f'go-phase-{offset}', 'fault-go', offset_ns=offset, gpio_ns=17, phase_ns=43)
# Own-cycle faults before and after acknowledgement/read capture. One-ns pulses
# are digital checks, not evidence that such pulses meet electrical specifications.
for fault in ('berr', 'bgack'):
    for writing in (False, True):
        for state in (3, 4, 5, 6):
            for width in (1, 63, 126, 252):
                case(f'{fault}-s{state}-w{width}-write{int(writing)}',
                     f'fault-{fault}' + ('-write' if writing else ''),
                     target_state=state, pulse_ns=width)
for state in range(8):
    for width in (252, 3000):
        case(f'reset-s{state}-w{width}', 'fault-reset-boundary', target_state=state, pulse_ns=width)
for scenario in ('late', 'commit', 'cancel', 'active-init', 'stale'):
    case(scenario, 'fault-' + scenario)
# Long command strobes must not multiply physical writes; alternate directions.
for gpio in (1, 17, 63, 80, 127, 250, 500, 1000):
    case(f'word-gpio-{gpio}', 'ram', gpio_ns=gpio, phase_ns=43)
    case(f'lanes-gpio-{gpio}', 'lanes', gpio_ns=gpio, phase_ns=17)
for gpio in (1, 17, 80):
    for delay in (0, 20, 50):
        case(f'settle-{gpio}-{delay}', 'lanes', gpio_ns=gpio, settle_ns=delay)
# The seed controls GPIO-operation jitter, device delays, DMA duration/ack delay,
# request target/offset, BERR, data, byte lanes and function codes.
for seed in range(16):
    case(f'random-{seed}', 'fault-random', seed=seed, gpio_ns=1+(17*seed)%250,
         jitter_ns=120, phase_ns=(seed*29)%126, settle_ns=(seed%3)*20, iterations=32)

summary = dict(success=False, cases=[], state_visits=[0]*8, request_states=[0]*8,
               event_phases=[0]*10, randomized_seeds=[], software_outcomes={},
               expected_cases=len(cases), simulator='Verilator 5.052', latch_model=args.latch_model)
failures = []
for index, (name, scenario, options) in enumerate(cases, 1):
    trace = args.output / (name + '.vcd')
    command = [str(args.runner), '--scenario', scenario, '--trace', str(trace),
               '--latch-model', args.latch_model]
    for key, value in options.items():
        command.extend(['--'+key.replace('_', '-'), str(value)])
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (args.output / (name + '.log')).write_text(result.stdout + result.stderr)
        report = json.loads(Path(str(trace)+'.json').read_text())
        ok = result.returncode == 0 and report['success']
        if 'source_manifest' not in summary:
            summary['source_manifest'] = report['manifest']
        if report['manifest'] != summary['source_manifest']:
            ok = False
        for key in ('state_visits', 'event_phases'):
            summary[key] = [a+b for a,b in zip(summary[key], report[key])]
        if scenario == 'fault-states':
            summary['request_states'] = [a+b for a,b in zip(summary['request_states'],report['event_states'])]
        if scenario == 'fault-random' and ok:
            summary['randomized_seeds'].append(options['seed'])
        if scenario in ('fault-late','fault-commit','fault-cancel','fault-active-init'):
            summary['software_outcomes'][scenario] = report['outcome']
        detail = (result.stderr.splitlines() or [''])[0]
    except (subprocess.TimeoutExpired, OSError, ValueError, KeyError) as exc:
        ok = False
        detail = str(exc)
    summary['cases'].append(dict(name=name, success=ok, command=shlex.join(command), error=detail if not ok else ''))
    if not ok:
        failures.append(name)
        print(f'FAIL {name}: {detail}\nReproduce: {shlex.join(command)}', flush=True)
    elif index % 10 == 0 or index == len(cases):
        print(f'PASS {index}/{len(cases)} ({name})', flush=True)
    # Persist intermediate results as well, so an interrupted campaign is visible.
    (args.output/'coverage.json').write_text(json.dumps(summary,indent=2)+'\n')

if not all(summary['state_visits']) or not all(summary['request_states']):
    failures.append('all-eight-state coverage')
if not all(summary['event_phases']):
    failures.append('all-ten-edge-phase coverage')
if summary['randomized_seeds'] != list(range(16)):
    failures.append('random seed coverage')
if summary['software_outcomes'] != {'fault-late':'committed','fault-commit':'committed',
                                   'fault-cancel':'aborted','fault-active-init':'aborted'}:
    failures.append('physical write outcome coverage')
summary['failures'] = failures
summary['success'] = not failures
(args.output/'coverage.json').write_text(json.dumps(summary,indent=2)+'\n')
print(f'{"FAIL" if failures else "PASS"}: {len(cases)} cases; failures={failures}; report={args.output/"coverage.json"}',flush=True)
sys.exit(bool(failures))
