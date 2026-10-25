# CPLD changes

## Development update — 2026-10-25

Applies the same transaction-engine fixes to the EPM240 and EPM570 1.1b
sources. The Pi wire protocol and `BusArbSync` state machine are unchanged.

- Capture each high-address request on the raw command-write rising edge.
  A long strobe no longer starts duplicate cycles/writes after completion.
  Reset/INIT and completion clear the request flip-flop; a strobe held across
  reset needs a fresh edge. Command changes while WR remains high cannot
  generate a phantom request.
- Require inactive DTACK/BERR before starting a cycle, preventing a foreign
  master's late acknowledgement from completing a fresh CPU access.
- Retain transient in-cycle BERR or illegal BGACK, report an error and release
  bus drivers instead of accepting foreign data as a successful transfer.
- Physical RESET and explicit INIT discard pending work without replay.
- Freeze the external write-data latch while an access is pending/active, so
  status/INIT preparation cannot overwrite a timed-out physical write.

A timeout/reset cannot undo a device write that was already accepted. The
test driver does not retry automatically.

Added standalone CPLD harnesses, mise build/test tasks, reconstructed Quartus
projects, revision includes, an ~8 MHz ST clock constraint, classic-board pin
audit, a Podman build recipe with the system-libudev compatibility fix, and
host simulation/fault campaigns.
The EPM240 project targets the tested EPM240T100I5; EPM570 targets EPM570T100C5.

The reconstructed projects follow physical GPIO nets: WR GPIO1/pin33,
IPL1 GPIO5/pin34, IPL2 GPIO6/pin35 and BERR GPIO7/pin29. Historical interface
signal names must not be used as substitutes for the board's physical wiring.

Firmware words remain `$421`/`$521` for compatibility. They do not identify the
patched image; use source and SVF hashes. Existing shipped SVFs are unchanged.
The H5 system in [TEST-SYSTEM.md](TEST-SYSTEM.md) boots EmuTOS and runs Gembench
from floppy with both the original-RTL rebuild and patched EPM240 RC1.
EPM570 is simulation/build-tested, not hardware-tested here. Asynchronous I/O
timing is still incompletely constrained; see [README.md](README.md).
