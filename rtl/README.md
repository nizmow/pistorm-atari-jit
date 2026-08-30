# rtl/ — the SXB CPLD design

## What this directory is

The PiStorm-SXB board's CPLD (EPM240/EPM570) is the hardware bridge
between the Pi and the Atari 68000 bus. This directory holds:

| File | What it is |
|---|---|
| `pistormsxb_dev_ng.v` | **Next-gen design** — the working-january-2025 CPLD RTL (bus arbitration + VPA/VMA IACK + S0–S7 68000 bus FSM) with the status word updated to match the JIT-era Pi software |
| `pistormsxb_dev_ng.qsf` | Quartus project pin assignments for the **SXB STe board** |
| `pistormsxb_dev_ng_classic.qsf` | Pin assignments for the **classic PiStorm board** (DIP-68, 373 latches) — the one this fork is developed on |
| `pistormsxb_dev_ng.sdc` | Timing constraints (M68K_CLK 8 MHz, everything async false-path) |
| `version_reg.v` | 11-bit firmware revision register (`include`d by the design) |
| `EPM240_bitstream*.svf` | Compiled bitstreams shipped by the author (current-generation firmware, 2026) |
| `pistormflash` | xsvftool-gpio — flashes the SVFs over bit-banged JTAG |

## Design lineage

1. **Oct 2023** (`gotaproblem/pistorm-atari` main branch): 200 MHz PI_CLK
   from the Pi, IPL returned in the status register, "IACK bus-cycle TODO".
2. **19 Jan 2025** (`jan2025` branch): PI_CLK removed; one-hot S0–S7
   68000 bus-cycle generator; VPA/VMA IACK handshake; BR/BG/BGACK bus
   arbitration. Author's header: *"Blitter is working, FDD is working,
   ACSI bus is working"*.
3. **NG (this file)**: jan2025 + status-word layout matching what the
   JIT-era software actually reads:
   - `PI_D[15:13]` = IPL (diagnostic, unchanged)
   - `PI_D[11]` = **BGACK** — the raw bus-grant-acknowledge observation
     that `ataritest --arbtest` reads via `ARB_STATUS_BUSY_RAW`
   - `PI_D[10:0]` = firmware revision — matches
     `ps_get_firmware_revision()` (`(status>>8) & 0x7FF`)

The current 2026 bitstreams (`EPM240_bitstream_dev*.svf`) are built from
a further-evolved private design; the NG RTL is the public reconstruction
of that interface, with the author's own working arbitration logic.

## Build (Quartus Prime 20.1.1 Lite — free, supports MAX II)

1. New project from `pistormsxb_dev_ng.qsf`
2. `Processing → Start Compilation` (EPM240T100C5 for the 240; change
   `DEVICE` to EPM570T100C5 for the 570)
3. Convert the .pof to SVF: `quartus_cpf -c pistormsxb_dev_ng.pof
   pistormsxb_dev_ng.svf`
4. Flash: `sudo ./pistormflash -s pistormsxb_dev_ng.svf`

## Per-board pin map — READ THIS BEFORE FLASHING

The Verilog port list is board-independent. **Only the .qsf changes per
board.** Two reference maps ship here:

- **`pistormsxb_dev_ng_classic.qsf`** — the CLASSIC board: the original
  Amiga PiStorm board (DIP-68 socket, EPM240, 74LVC16373 transparent
  latches — 373-type). This is the board the JIT README calls "PiSTorm
  board (Amiga A500) for DIL 68pin CPUs (STfm, MST)" and the one this
  fork was developed on. The CPLD pins are the original project's
  assignments; the signals are the **2026-generation rotation** the
  JIT-era `ps_protocol.h` (non-PI_CLK) expects. **This is almost
  certainly the qsf you want.**
- **`pistormsxb_dev_ng.qsf`** — the SXB STe board reference (jan2025
  generation wiring: IPL1/2 on GPIO1/4, BERR on GPIO5, RD/WR on GPIO6/7).

The rotation on the classic board's fixed wires (GPIO ↔ CPLD pin):

```
GPIO0↔PIN_30  TXN          GPIO4↔PIN_12  RD   (was 200MHz PI_CLK!)
GPIO1↔PIN_33  WR           GPIO5↔PIN_34  IPL1 (was RESET/BERR)
GPIO2↔PIN_27  A0           GPIO6↔PIN_35  IPL2 (was RD)
GPIO3↔PIN_28  A1           GPIO7↔PIN_29  BERR (was WR)
GPIO8-23↔PIN_2-26  D[15:0]   GPIO24-27↔PIN_22-25  JTAG
```

No 200 MHz PI_CLK is generated or used (the JIT build does not define
`USING_PI_CLK`, and the jan2025-generation RTL runs on the Atari's
M68K_CLK). 373 and 374 latches both work — jan2025 was field-tested with
both; your 373s are the original board's part.

## Verification status

- [x] RTL syntax-verified (pyverilog parse, 2026-08-30)
- [x] Status-word layout cross-checked against `ps_protocol.c` /
      `ataritest.c` (`ARB_STATUS_BUSY_RAW`, `ps_get_firmware_revision`)
- [ ] Quartus fit (EPM240 = 240 LEs — the jan2025 design fits; NG adds
      ~0 LEs, only a mux change)
- [ ] On-hardware: flash + `ataritest --arbtest --source=blitter` to
      confirm BGACK appears on status bit 11, then real blitter/FDD/ACSI

## The story in one paragraph

The author fixed bus arbitration in the CPLD by January 2025 (blitter,
FDD and ACSI all working) and then stopped sharing the RTL. The jan2025
source + the JIT-era software's protocol expectations let us reconstruct
the current interface exactly — and the NG design is the base for
whatever comes next: fixing the autovector IACK handshake properly,
routing the BGACK observation to the Pi (done — status bit 11), or
adding external-write coherence for the natmem mirror.
