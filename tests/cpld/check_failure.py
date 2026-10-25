#!/usr/bin/env python3
"""Exercise the real assertion failure path, including equal-value contention."""
from pathlib import Path
import subprocess
import sys

runner, trace = sys.argv[1:]
result = subprocess.run([runner, '--scenario', 'ram', '--trace', trace,
                         '--inject-contention'], text=True, capture_output=True)
if result.returncode != 1 or 'PI_D contention: injected equal-value driver' not in result.stderr:
    raise SystemExit(f'Expected contention failure:\n{result.stdout}\n{result.stderr}')
if 'Reproduce:' not in result.stderr or 'seed=1' not in result.stdout or 'rtl_sha256' not in result.stdout:
    raise SystemExit('Missing reproduction metadata')
vcd = Path(trace).read_text()
for token in ('$enddefinitions', 'sim_pi_gpio_mask', 'sim_st_address', '#'):
    if token not in vcd:
        raise SystemExit(f'Missing waveform signal/record: {token}')
print('PASS equal-value contention rejected; waveform and reproduction metadata retained')

sim_trace = trace + '.fatal.vcd'
result = subprocess.run([runner, '--scenario', 'ram', '--trace', sim_trace,
                         '--inject-simulator-failure'], text=True, capture_output=True)
if result.returncode != 1 or 'simulator fatal:' not in result.stderr or 'Reproduce:' not in result.stderr:
    raise SystemExit(f'Simulator fatal did not unwind/report correctly:\n{result.stderr}')
if '$enddefinitions' not in Path(sim_trace).read_text():
    raise SystemExit('Simulator failure did not leave a readable trace')
print('PASS simulator fatal retains waveform and reproduction command')
