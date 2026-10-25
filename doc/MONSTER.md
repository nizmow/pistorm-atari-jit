# MonSTer Alt-RAM

`monster true` emulates the RAM interface of an original MonSTer expansion:
up to 8 MB of Pi-backed Alt-RAM at `$00400000–$00BFFFFF`. It works with an
emulated 68000 or 68010 and keeps their 24-bit address wrapping. It can also
be used with later CPUs, alongside the separate `ttram` mapping above 16 MB.
The default is off.

Add this to the desired section of `psctrl.cfg` for a 68000 configuration:

```text
cpu 68000
jit false
ttram false
monster true
```

ST-RAM size and the physical screen remain independent of this option.
This is CPU memory: the real Atari Shifter and physical DMA cannot use it.
The emulated blitter can read and write it. For real blitter hardware, leave
Alt-RAM blits disabled in guest software. Configurations with emulated ET4000
windows or `machine falcon`/expanded Falcon ST-RAM disable MonSTer with a
message at startup because their address spaces can overlap. fVDI's framebuffer
is outside this aperture and is allowed.

## Guest software

* Use a standard EmuTOS build with MonSTer support (normally the larger ROM
  builds). It detects the register and registers the RAM during startup;
  no AUTO program is needed. An ARAnyM-specific build may skip MonSTer
  detection; use an ST-compatible ROM.
* For TOS 2.06 or MagiC, download the original
  [MonSTer tools](https://atari.joska.no/monster/monster.zip).
  Run `M_ALTRAM.PRG` from the desktop to configure 2, 4, 6 or 8 MB, then put
  the configured program in `AUTO`. Under MagiC, run it after `MAGXBOOT.PRG`.
  Consult the [utility author's instructions](https://atari.joska.no/monster/).
  The utility is not redistributed in this repository.
* Stock TOS 1.x lacks the required Alt-RAM memory manager. Geneva alone does
  not add it. Use EmuTOS, TOS 2.06 or MagiC to allocate this memory.
* Storm ST's `ALTRAMON.PRG` has a different detection sequence and is not a
  supported installer for this mode.

The guest OS/program-header flags determine which applications load into
Alt-RAM and whether ordinary allocations may use it. Screen and physical DMA
buffers must remain in ST-RAM. EmuTOS and `M_ALTRAM.PRG` provide an `_FRB`
staging buffer for compatible disk drivers.

## Emulated interface

The model implements the protocol used by EmuTOS's `bios/memory2.c` and by
`M_ALTRAM.PRG`'s `set_monster` routine:

1. Read `$FFFFFE00` (or its 24-bit address `$00FFFE00`).
2. Write the requested size in megabytes to that word twice.
3. The second matching write enables 0, 2, 4, 6 or 8 MB at `$00400000`.

The RAM starts disabled. Reads return the active size in the low nibble;
invalid or unmatched writes do not change the mapping. RAM contents survive
size changes and the guest's RESET instruction. A full emulator restart
starts disabled again. Guest software must not shrink/disable RAM that GEMDOS
has registered or that programs are using.

`$FFFE02/04/06` absorb I2C writes and return floating SDA (no attached clock).
The optional firmware register at `$FFFE08` and the MonSTerbo probe at
`$FFFE0A` produce guest bus errors: this is an original MonSTer, with no flash,
RTC, CPU-speed control or MonSTerbo features. The Mega STE installer may
limit the allocation to 6 MB, matching its original-firmware restrictions.

Enabled RAM uses its own CPU bank and 24-bit aliases. Stores invalidate
translated code through the existing mirror-only RAM handlers. Mapping changes
request a CPU exit and flush translations outside compiled code. NatFeat host
RAM access accepts the active region, without treating it as DMA-capable ST-RAM.

## Checks

```sh
sh tests/monster/run.sh
# Also execute the downloaded utility's original control routine on Musashi:
sh tests/monster/run.sh /path/to/M_ALTRAM.PRG
sh platforms/atari/psctrl/run-test.sh
# On an AArch64 build host with the emulator dependencies installed:
make monster-bank-test
./monster-bank-test
```

The optional binary test recognizes the inspected 17,875-byte utility and
executes its actual control routine for all five sizes on an emulated 68000.
The bank integration test links the complete emulator and checks all 256
address aliases, CPU stores, instruction eligibility, blitter writes, NatFeat
writes and code invalidation, and disabling/re-enabling RAM. It can also run
under `qemu-aarch64` with an ARM64 userspace. For a cross build, set `CC`,
`CXX` and `OBJCOPY` to the AArch64 toolchain.

Hardware testing reported by the PiStorm user:

* EmuTOS detects and registers 8 MB; SysInfo displays it as TT-RAM.
* An IDE-booted Geneva/NeoDesk desktop with NVDI runs with Alt-RAM in use
  and more free ST-RAM than before.
* GEMBench's Alt-RAM test passes.

The automated checks do not run the complete M_ALTRAM.PRG GUI/installer or
boot TOS. The full installer under TOS 2.06/MagiC, execution addresses of
individual applications, physical DMA transfers and warm-reset behaviour
remain unverified.

References: [EmuTOS detection](https://github.com/emutos/emutos/blob/master/bios/machine.c),
[EmuTOS registration](https://github.com/emutos/emutos/blob/master/bios/memory2.c),
[MonSTer utility documentation](https://atari.joska.no/monster/).
