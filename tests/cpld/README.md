# CPLD co-simulation

Native C/C++ tests drive the actual EPM240 and EPM570 Verilog independently.
The C wire driver in `driver/` is a portable test-side implementation of the
PiStorm Atari register/GPIO protocol.
It is not linked into the emulator and is not the production JIT driver.
No other checkout or firmware build is required.

From the repository root:

```sh
mise install
mise run test:cpld -j 3
# Focused runs:
mise run test:cpld:arbiter
mise run test:cpld:faults
```

Without mise, use `bash tests/cpld/run.sh` with the same CTest arguments.
Mise pins CMake 4.1.2 and Ninja 1.13.1. Other prerequisites: Python 3, Git
and a C/C++17 compiler (CMake >=3.20 and Ninja if installed without mise).
Bootstrapping Verilator also needs make, autoconf, flex, bison and Perl (Ubuntu:
`build-essential cmake ninja-build python3 git autoconf flex bison perl`).
The first run clones/builds Verilator **5.052**, source commit
`ea338be98e1e838d3518809ce8899f85a009963c`, under `.tools/` and needs network.
Compiler/system libraries are not pinned. Set `CPLD_VERILATOR_ROOT` to an existing source-tree build of
that exact revision, `CPLD_TEST_BUILD_DIR` to change the output directory,
or `CPLD_BUILD_JOBS` to change compilation parallelism (default 2).

## Coverage

The 36 CTest entries include:

- A 59-case isolated `BusArbSync` campaign per board: four states, all 12
  reachable transitions, reset from every state/settling phase, request
  withdrawal and interruption. Changes are swept -2/-1/0/+1/+2 ns around
  rising and falling edges, including sampled/missed short requests.
- RAM words, byte lanes, direction changes, delayed DTACK, BERR, control/status
  writes, DMA write/read and pending/active ownership boundaries.
- A raw-strobe regression: hold WR across completion, change commands without
  a WR edge, reset while WR remains high, and issue a fresh request after release.
- A 164-case full-engine campaign per board, plus a separate 164-case EPM240
  campaign with transparent 16373A external latches. It covers all eight transfer
  states and ten edge-phase bins, BERR/BGACK pulses, reset/INIT, stale DTACK,
  long strobes, delayed read-latch visibility, timeout/cancel/late-write outcomes,
  and 16 deterministic seeds of 32 write attempts. Every case must pass and
  coverage gates enforce state/phase/seed/outcome coverage.
- Intentional equal-value contention and simulator-fatal tests, which must
  reject the fault and retain readable waveforms/reproduction commands.

The bus model has 64 KiB deterministic RAM, a synthetic peripheral at `$ff0000`,
and delayed responders. It checks enabled drivers independently of driven
values, so two drivers conflict even when their values agree. Monitors check
GPIO and ST bus contention, latch ownership, address/data/FC/byte lanes,
exactly-once completion and foreign-master driver release.

## Model and provenance

Time advances in 1 ns ticks with a 126 ns ST clock. Each GPIO read/write/barrier
advances simulated time; there are no wall-clock protocol delays. External
16374 registers capture on rising edges by default. `--latch-model 16373`
models transparent data while LE is high, retaining data when it falls.
Both use the RTL's existing `LS374` compatibility timing. The alternative
RTL `LS373` timing mode is not tested.

`prepare_rtl.py` creates simulator copies without editing source RTL:

1. Replace the single zero-delay `always begin` process with `always @(*)`.
   Incomplete assignments retain storage; no defaults or alternate protocol
   logic are added. Verilator executes combinational nonblocking assignments
   as blocking assignments, as synthesis does.
2. Split Pi inout and RESET/HALT driver intent from receivers for explicit
   resolution and contention checks; expose existing bus-enable/ownership signals.
3. Add trace-only resolved bus/latch/driver-mask inputs.

`LATCH`, `COMBDLY` and `CASEINCOMPLETE` warnings are explicitly allowed; other
Verilator warnings remain fatal. The arbiter probe instantiates `BusArbSync`
from the original source, without transaction-module adaptations. Both board
files must remain logically identical except for top-module/revision-include
names; the fixture generator checks that assumption. Actual revision includes
from `rtl/` are used, not replacement version fixtures.

Each board's manifest records RTL revision/hash, adapted hash, version include,
wire-driver, harness, probe and arbiter-runner hashes. Campaign summaries retain
each command/result in `epm*/faults/coverage.json`; successful VCDs are removed,
failure VCDs retained. CTest output is in `Testing/Temporary/LastTest.log`.
Reproduce an individual case with the command in its log, for example:

```sh
build/cpld-tests/rtl_240 --scenario dma --seed 19 --gpio-ns 17 --phase-ns 43 \
  --latch-model 16373 --trace /tmp/atari-dma.vcd --keep-trace
```

This is a settled digital logic model, not proof of physical asynchronous
hazard behavior, delta-cycle equivalence, setup/hold, metastability, voltage
levels or electrical bus turnaround. Foreign owners are assumed to release
the bus and responding devices eventually acknowledge. VPA/E-clock peripheral
handshakes, analog behavior and full-emulator startup are not modeled.
A timeout never proves a physical write was cancelled; there are no automatic
retries. Hardware results and remaining Quartus timing limits are documented
in [rtl/TEST-SYSTEM.md](../../rtl/TEST-SYSTEM.md) and [rtl/README.md](../../rtl/README.md).
