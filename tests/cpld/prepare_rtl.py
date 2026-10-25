#!/usr/bin/env python3
"""Mechanical simulation-only adaptations; never edit or replace protocol logic."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

source, output, variant = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
original = source.read_text()
if variant == '570':
    # The checked-in variants currently share all logic. Fail visibly if that
    # assumption changes rather than accidentally testing one board twice.
    other = source.with_name('pistormSXB_devEPM240_1.1b.v').read_text()
    def shared(s):
        s = re.sub(r'/\*.*?\*/|//[^\n]*', '', s, flags=re.S)
        return re.sub(r'\s+', '', s.replace('EPM570', 'EPM240').replace('version_reg_570', 'version_reg'))
    if shared(original) != shared(other):
        raise SystemExit('EPM240/EPM570 shared-logic assumption changed; audit the fixtures')
text = original

def replace(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f"RTL fixture drift: expected exactly one {old!r} in {source}")
    text = text.replace(old, new)

# A zero-delay infinite process has no event simulation meaning. Quartus infers
# transparent storage from incomplete assignments. @* preserves that storage;
# do NOT add defaults or turn these assignments into edge-triggered flops.
replace('always begin', 'always @(*) begin')
# Separate physical drivers from receivers. The C++ resolver checks each enable
# before selecting data, including equal-value contention and partial GPIO FSEL.
replace('inout   [15:0]  PI_D,', '''input [15:0] PI_D,
    input [15:0] sim_pi_gpio_mask,
    input [15:0] sim_pi_gpio_value,
    input [15:0] sim_read_latch_value,
    input [15:0] sim_write_latch_value,
    input [15:0] sim_st_data,
    input [23:0] sim_st_address,
    input sim_dma_drive,
    input sim_ram_drive,
    output [15:0] sim_status_data,
    output sim_status_oe,
    output [7:0] sim_states,
    output sim_reset_sm,
    output sim_bus_drv,
    output sim_bus_mine,
    output sim_go,
    output sim_as_on,
    output sim_bgack_i,
    output sim_reset_drive,
    output sim_halt_drive,''')
text, n = re.subn(r'assign PI_D\s*= trigger \? (.*?) : 16\'bz;',
                  r'assign sim_status_data = \1;\n assign sim_status_oe = trigger;', text)
if n != 1:
    raise SystemExit('RTL fixture drift: status driver')
replace('inout           M68K_RESET_n,', 'input           M68K_RESET_n,')
replace('inout           M68K_HALT_n,', 'input           M68K_HALT_n,')
text, n = re.subn(r'assign M68K_RESET_n\s*= \(reset_out\) \? LO : 1\'bz;',
                  'assign sim_reset_drive = reset_out;', text)
if n != 1:
    raise SystemExit('RTL fixture drift: reset driver')
text, n = re.subn(r'assign M68K_HALT_n\s*= \(halt\) \? LO : 1\'bz;',
                  'assign sim_halt_drive = halt;', text)
if n != 1:
    raise SystemExit('RTL fixture drift: halt driver')
# Expose the actual RTL enable separately from its driven control values.
text, n = re.subn(r'(assign M68K_(?:FC|AS_n|UDS_n|LDS_n|RW|VMA_n)\s*=) bus_drv \? (\w+)\s*: [13]\'b[ z]+;',
                  r'\1 \2;', text)
if n != 6:
    raise SystemExit(f'RTL fixture drift: {n} control drivers, expected 6')
# Insert instrumentation into the transaction module only.
pos = text.rfind('endmodule')
text = text[:pos] + '''
 assign sim_states = {s7,s6,s5,s4,s3,s2,s1,s0};
 assign sim_reset_sm = reset_sm;
 assign sim_bus_drv = bus_drv;
 assign sim_bus_mine = bus_mine;
 assign sim_go = go;
 assign sim_as_on = as_on;
 assign sim_bgack_i = BGACKi;
''' + text[pos:]
output.mkdir(parents=True, exist_ok=True)
def write_changed(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)
write_changed(output / 'cpld.v', text)
revision = subprocess.check_output(['git', '-C', str(source.parent), 'rev-parse', 'HEAD'], text=True).strip()
arbiter_module = re.search(r'module BusArbSync\b.*?endmodule', original, re.S)
if not arbiter_module:
    raise SystemExit('BusArbSync module missing from original RTL')
manifest = dict(fixture_version=3,
                harness_sha256=hashlib.sha256((Path(__file__).parent / 'harness.cpp').read_bytes()).hexdigest(),
                arbiter_module_sha256=hashlib.sha256(arbiter_module.group().encode()).hexdigest(),
                arbiter_probe_sha256=hashlib.sha256((Path(__file__).parent / 'arbiter_probe.sv').read_bytes()).hexdigest(),
                arbiter_test_sha256=hashlib.sha256((Path(__file__).parent / 'arbiter.cpp').read_bytes()).hexdigest(),
                version_includes_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(source.parent.glob('version_reg*.v'))},
                variant=variant, rtl_path=str(source), rtl_revision=revision,
                rtl_sha256=hashlib.sha256(original.encode()).hexdigest(),
                adapted_sha256=hashlib.sha256(text.encode()).hexdigest(),
                engine_sha256=hashlib.sha256((Path(__file__).parent / 'driver/ps_atari_bus.c').read_bytes()).hexdigest())
write_changed(output / 'manifest.json', json.dumps(manifest, indent=2) + '\n')
