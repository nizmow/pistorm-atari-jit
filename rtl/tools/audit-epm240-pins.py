#!/usr/bin/env python3
"""Compare QSF and fitted pins with the original classic Rev B PCB netlist.

Supply the upstream PiStormRevB_ascii.zip; no network or hardware access occurs.
This checks a reference board, not the identity/continuity of a physical PCB.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--board-archive', type=Path, required=True)
parser.add_argument('--qsf', type=Path, default=Path(__file__).resolve().parents[1] / 'pistormsxb_devEPM240.qsf')
parser.add_argument('--pin-report', type=Path, required=True)
parser.add_argument('--output', type=Path)
args = parser.parse_args()

board_sha = hashlib.sha256(args.board_archive.read_bytes()).hexdigest()
if board_sha != '2ac6a486496698de8b84f46b0bc3b8141f430bc506c9984b4378929122aa2188':
    raise SystemExit('Board archive differs from the reviewed upstream Rev B snapshot; audit it first')

with zipfile.ZipFile(args.board_archive) as archive:
    pcb = archive.read('Pistorm_rev_B_ascii.PcbDoc').decode()
records = [dict(field.split('=', 1) for field in line.split('|') if '=' in field)
           for line in pcb.splitlines()]
nets = {r['ID']: r['NAME'] for r in records if r.get('RECORD') == 'Net'}
components = {r['ID']: r['SOURCEDESIGNATOR'] for r in records if r.get('RECORD') == 'Component'}
pads = {}
for r in records:
    if r.get('RECORD') == 'Pad' and r.get('COMPONENT') in components:
        pads.setdefault(components[r['COMPONENT']], {})[r['NAME']] = nets.get(r.get('NET'))
cpld = pads['U4']

# Pi header physical pins -> BCM GPIO numbers, independent of QSF/RTL names.
header = {27: 0, 28: 1, 3: 2, 5: 3, 7: 4, 29: 5, 31: 6, 26: 7,
          24: 8, 21: 9, 19: 10, 23: 11, 32: 12, 33: 13, 8: 14, 10: 15,
          36: 16, 11: 17, 12: 18, 35: 19, 38: 20, 40: 21, 15: 22, 16: 23,
          18: 24, 22: 25, 37: 26, 13: 27}
gpio_nets = {g: pads['RPI_ZERO'][str(h)] for h, g in header.items()}
signals = {0: 'PI_TXN_IN_PROGRESS', 1: 'PI_CMD_WR', 2: 'PI_CMD[0]', 3: 'PI_CMD[1]',
           4: 'PI_RESET', 5: 'PI_IPL1', 6: 'PI_IPL2', 7: 'PI_BERR'}
signals.update({i + 8: f'PI_D[{i}]' for i in range(16)})
signal_nets = {s: gpio_nets[g] for g, s in signals.items()}
signal_nets.update({
    'LTCH_A_0': 'ADDR_LTCH_0', 'LTCH_A_8': 'ADDR_LTCH_8',
    'LTCH_A_16': 'ADDR_LTCH_16', 'LTCH_A_24': 'ADDR_LTCH_24',
    'LTCH_A_OE_n': 'ADDR_OE', 'LTCH_D_RD_U': 'DATA_RD_LTCH_HB',
    'LTCH_D_RD_L': 'DATA_RD_LTCH_LB', 'LTCH_D_RD_OE_n': 'DATA_RD_OE',
    'LTCH_D_WR_U': 'DATA_WR_LTCH_HB', 'LTCH_D_WR_L': 'DATA_WR_LTCH_LB',
    'LTCH_D_WR_OE_n': 'DATA_WR_OE', 'M68K_CLK': pads['R3']['1'],
})
# Trace 68000 control nets through the two SN74CBTD3384 level shifters.
bus_signals = {'VMA': 'M68K_VMA_n', 'E': 'M68K_E', 'VPA': 'M68K_VPA_n',
               'BERR': 'M68K_BERR_n', 'RESET': 'M68K_RESET_n', 'HALT': 'M68K_HALT_n',
               'BR': 'M68K_BR_n', 'BGACK': 'M68K_BGACK_n', 'BG': 'M68K_BG_n',
               'AS': 'M68K_AS_n', 'UDS': 'M68K_UDS_n', 'LDS': 'M68K_LDS_n',
               'R/W': 'M68K_RW', 'DTACK': 'M68K_DTACK_n'}
bus_signals.update({f'IPL{i}': f'M68K_IPL_n[{i}]' for i in range(3)})
bus_signals.update({f'FC{i}': f'M68K_FC[{i}]' for i in range(3)})
for ref in ('U1', 'U7'):
    for a, b in ((2, 3), (5, 4), (6, 7), (9, 8), (10, 11),
                 (15, 14), (16, 17), (19, 18), (20, 21), (23, 22)):
        signal_nets[bus_signals[pads[ref][str(b)]]] = pads[ref][str(a)]
expected = {}
for signal, net in signal_nets.items():
    pins = [p for p, n in cpld.items() if n == net]
    if len(pins) != 1:
        raise SystemExit(f'Ambiguous PCB net for {signal}: {net}: {pins}')
    expected[signal] = pins[0]
assert len(expected) == 56

qsf = args.qsf.read_text()
assignments = re.findall(r'^set_location_assignment PIN_(\d+) -to (\S+)\s*$', qsf, re.M)
declared = {s: p for p, s in assignments}
errors = []
if len(assignments) != len(declared) or len({p for p, s in assignments}) != len(assignments):
    errors.append('Duplicate QSF signal or pin assignment')
if declared != expected:
    errors.extend(f'QSF {s}: expected {expected.get(s)}, got {declared.get(s)}'
                  for s in sorted(expected.keys() | declared.keys()) if expected.get(s) != declared.get(s))
device = re.search(r'^set_global_assignment -name DEVICE (\S+)', qsf, re.M)
if not device or device[1] != 'EPM240T100I5':
    errors.append('Expected EPM240T100I5 project target')

report = args.pin_report.read_text()
if not re.search(r'ASSIGNED TO AN: EPM240T100I5\b', report):
    errors.append('Fitted report does not target EPM240T100I5')
fitted, package = {}, {}
for line in report.splitlines():
    fields = [x.strip() for x in line.split(':')]
    if len(fields) == 7 and fields[1].isdigit():
        name, pin, direction, standard, voltage, bank, user = fields
        package[pin] = name
        if direction in ('input', 'output', 'bidir') and pin not in ('22', '23', '24', '25'):
            fitted[name] = pin
            if standard != '3.3-V LVTTL' or user != 'Y':
                errors.append(f'{name}: IO standard/user assignment: {standard}/{user}')
            inputs = {'PI_CMD_WR', 'PI_CMD[0]', 'PI_CMD[1]', 'M68K_CLK', 'M68K_DTACK_n',
                      'M68K_BERR_n', 'M68K_VPA_n', 'M68K_BR_n', 'M68K_BGACK_n',
                      'M68K_IPL_n[0]', 'M68K_IPL_n[1]', 'M68K_IPL_n[2]'}
            wanted = ('bidir' if name.startswith('PI_D[') or name in ('M68K_RESET_n', 'M68K_HALT_n')
                      else 'input' if name in inputs else 'output')
            if direction != wanted:
                errors.append(f'{name}: expected direction {wanted}, got {direction}')
if fitted != expected:
    errors.extend(f'Fitted {s}: expected {expected.get(s)}, got {fitted.get(s)}'
                  for s in sorted(expected.keys() | fitted.keys()) if expected.get(s) != fitted.get(s))
for pin in ('22', '23', '24', '25'):
    if package.get(pin) != cpld[pin]:
        errors.append(f'JTAG pin {pin}: expected {cpld[pin]}, got {package.get(pin)}')
# Rev B EPM570 power pads become extra user IO on EPM240. Never drive them.
for pin in ('37', '39', '88', '90'):
    if package.get(pin) != 'RESERVED_INPUT_WITH_WEAK_PULLUP':
        errors.append(f'Board rail pad {pin} must remain a reserved input')

result = dict(success=not errors, errors=errors, signal_count=len(expected),
              board_url='https://github.com/captain-amygdala/pistorm/blob/main/PiStormRevB_ascii.zip',
              board_archive_sha256=board_sha,
              qsf_sha256=hashlib.sha256(args.qsf.read_bytes()).hexdigest(),
              pin_report_sha256=hashlib.sha256(args.pin_report.read_bytes()).hexdigest(),
              expected_pins=expected)
if args.output:
    args.output.write_text(json.dumps(result, indent=2) + '\n')
print(f'{"PASS" if result["success"] else "FAIL"}: {len(expected)} signal pins, JTAG, rail-connected unused IO')
for error in errors:
    print(error)
raise SystemExit(bool(errors))
