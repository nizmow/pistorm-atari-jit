# PROTOCOL-DESIGN — the Pi ↔ Atari (C/Pi) interface

How the Raspberry Pi talks to the Atari through the PiStorm CPLD: the GPIO
wire protocol (`gpio/ps_protocol.c`), the memory/bus dispatch that sits on
top of it, and the two genuinely thorny subsystems — **interrupts** and
**bus arbitration**.

This doc is derived from the code in this tree (HEAD of
`gotaproblem/pistorm-atari-jit`). Where a fact is inferred rather than
stated in code, it is labelled **[inferred]**.

**CPLD firmware source:** the current 2026-generation RTL is private, but
two older generations of the same design are public in
`gotaproblem/pistorm-atari` (see §1.1) — including working bus
arbitration and the full SXB pin map — and the compiled bitstreams ship
in this repo's `rtl/`, flashed with `rtl/pistormflash`.

---

## 1. Topology

```
        Raspberry Pi                          PiStorm board
   ┌───────────────────┐        GPIO      ┌──────────────────────┐
   │ emulator / JIT    │                 │        CPLD          │    Atari ST bus
   │ (cpu thread,      │◄───────────────►│  (EPM240/EPM570)     │◄──► 68000 socket:
   │  ipl_task, fdd…)  │  24 GPIO lines  │  addr/data latches   │     A1-A23, D0-D15,
   └───────────────────┘                 │  (373/374 octal)     │     AS, UDS/LDS, R/W,
                                         │  state machine       │     DTACK, BERR, VPA,
                                         └──────────────────────┘     IPL0-2, BR/BG/BGACK,
                                                                      E, ROM/RAM decode…
```

The Pi emulates the 68000 (JIT-translated to AArch64) and *becomes* the
bus master of the Atari. Every guest memory access that is not served from
the Pi's own RAM mirror ("natmem") is turned into a GPIO transaction that
the CPLD executes on the real Atari bus.

Files that matter:

| Piece | File |
|---|---|
| Wire protocol (transactions, status, IPL/BERR reads) | `gpio/ps_protocol.c`, `gpio/ps_protocol.h` |
| UAE bank handlers / natmem mirror / bus dispatch | `pistorm_natmem.cpp` |
| IPL sampler thread, VBL/FDD/stbox housekeeping | `emulator.c` (`ipl_task`, `wait_ns`) |
| Interrupt acknowledge (bus IACK), vector selection | `jit_glue.cpp` (`intlev_ack`), `cpu/newcpu.cpp` (`do_interrupt`, `effective_interrupt_vector`, `Exception_normal` patch) |
| Arbitration diagnostic | `ataritest.c` (`arbTest`) |
| SMI (shared-memory) variant — **not used by the Atari build** | `gpio/smi/ps_protocol_smi.c` (build links `gpio/ps_protocol.c` only, `Makefile:29`) |

### 1.1 The CPLD design: source is PUBLIC (two generations), bitstreams ship in-tree

The current 2026-generation RTL (the source of the in-tree SVFs below) is
private, but **two earlier generations of the exact same design are public**
in the author's pre-JIT repo `gotaproblem/pistorm-atari`:

| Generation | Where | Highlights |
|---|---|---|
| **Oct 2023** (`main` branch, `rtl/pistormSXB_devEPM240.v`) | public | 200 MHz PI_CLK from the Pi, IPL returned in the status register (bits 13–15), "IACK bus-cycle TODO" in the S4 state |
| **Jan 2025** (`jan2025` branch, same filename) | public | PI_CLK **removed** (runs on the Atari's 8 MHz only), one-hot S0–S7 68000 bus-cycle generator, VPA/VMA IACK handshake implemented, firmware-revision register (`version_reg.v` include), header claims **"Blitter is working, FDD is working, ACSI bus is working"** — i.e. arbitration was functional by Jan 2025 |

Both variants (EPM240 and EPM570) are in `rtl/`, plus the Quartus project
files: `pistormsxb_devEPM240.qsf` (pin assignments — **the SXB board pin
map**) and `pistormsxb_devEPM240.sdc` (timing constraints). The board
hardware is public too: `hardware/pistorm-ste/kicad/` (PiStormSTE with
PLCC68 adapter) has the schematic/PCB that shows the Pi-GPIO ↔ CPLD ↔
68000-socket wiring. Licence: MIT (Copyright 2021 PiStorm developers) —
iterate freely with attribution.

**Interface stability across generations** — verified by comparing all
three generations' decodes:

- The **transaction payload encoding never changed**: REG_DATA /
  REG_ADDR_LO / REG_ADDR_HI / REG_STATUS on PI_A[1:0]; upper address on
  D0–7, byte/read type bits on D8/D9, FC on D13–15 in the ADDR_HI phase;
  `op_fc = PI_D[15:13]`, `op_rw = PI_D[9]`, `op_uds/lds = PI_D[8] ? a0 : 0`.
- What **did** change is the control-pin assignment: Oct 2023 and Jan 2025
  use RD=GPIO6/WR=GPIO7/BERR=GPIO5 and sample IPL via the status register;
  the 2026 JIT-era interface (`ps_protocol.h`, and `ipl_task` reading
  GPIO5/6 directly) uses WR=GPIO1/RD=GPIO4/IPL1=GPIO5/IPL2=GPIO6/
  BERR=GPIO7 and reads IPL from dedicated pins. The physical board
  revision for the 2026 generation must match the current header — one
  continuity probe settles it.

**Firmware artifacts in this repo** (`rtl/`): ten `.svf` bitstreams, all
for Quartus project `pistormsxb_devEPM240` (EPM240), one per build:

| File | .pof built | Notes |
|---|---|---|
| `EPM240_bitstream_dev-3.svf` | 2026-04-26 | earliest in-tree build |
| `EPM240_bitstream_dev_065a_works_260626.svf` | 2026-06-26 | "065a works" |
| `EPM240_bitstream_dev_works_010726.svf` | 2026-07-01 | "works" |
| `EPM240_bitstream_dev-190726.svf` | 2026-07-18 08:16 | |
| `EPM240_bitstream_dev_010826.svf` | 2026-07-18 08:16 | **byte-identical to dev-190726** (misnamed copy) |
| `EPM240_bitstream_dev_210726.svf` | 2026-07-21 | |
| `EPM240_bitstream_dev_230726.svf` | 2026-07-23 | |
| `EPM240_bitstream_dev.svf` | 2026-08-15 | **newest build** |
| `EPM240_bitstream_dev_works.svf` | (see header) | "works" |
| `EPM240_bitstream_dev_dev_jit.svf` | (see header) | JIT-development build |

Flashing: `rtl/pistormflash` is **xsvftool-gpio** (libxsvf) — it bit-bangs
JTAG (TDI/TDO/TCK/TMS) over raw GPIO, e.g. `pistormflash -s
EPM240_bitstream_dev.svf`. Nothing in the build/install scripts references
the SVFs; flashing is a manual step. The nine unique images differ by
configuration bits only — a byte-level diff quantifies *how much* the CPLD
logic changed per build, but without the Quartus fitter project the
bitstream cannot be decompiled to logic, so which build added what (e.g.
arbitration work) is not readable from the SVFs alone. The `dev.svf`
(Aug 15) build post-dates the arbTest work in `ataritest.c`, which is the
right order of events for arbitration support being actively developed.

---

## 2. GPIO map (Atari build — no `USING_PI_CLK`)

The Makefile does **not** define `USING_PI_CLK`, so the Atari pinout is the
plain variant from `ps_protocol.h`. All three function-select registers are
written once in `ps_setup_protocol()` (`GPFSEL0/1/2_OUTPUT`); direction
never changes at runtime.

| GPIO | Direction | Name | Meaning |
|---|---|---|---|
| 0 | in | `PI_TXN_IN_PROGRESS` | CPLD holds high for the duration of a transaction; low = complete |
| 1 | out | `PIN_WR` | write strobe (CPLD latches on it) |
| 2 | out | `PI_CMD[0]` | command/register select, bit 0 |
| 3 | out | `PI_CMD[1]` | command/register select, bit 1 |
| 4 | out | `PIN_RD` | read strobe (CPLD drives data bus while high) |
| 5 | in | `PI_IPL1` | encoded IPL bit 1 (active-high, CPLD-inverted) |
| 6 | in | `PI_IPL2` | encoded IPL bit 2 (active-high, CPLD-inverted) |
| 7 | in | `PI_BERR` | bus error, **active low** (`CHECK_BERR`: low ⇒ error) |
| 8–23 | out (bus) | `D0–D15` | multiplexed address/data bus (see below) |
| — | — | — | — |
| 19 | out† | raw BGACK observation | `ARB_STATUS_BUSY_RAW = 1<<19` in ataritest — see §8 |

`USING_PI_CLK` (an alternative pinout used by other PiStorm forks) moves
WR/RD to GPIO 6/5, adds a 125 MHz clock on GPIO 4, and brings out a third
interrupt line `PI_IPL_ZERO` on GPIO 1 — evidence that the CPLD *can*
expose the raw low-order IPL bit, but the Atari build does not use it
(levels are even-only, see §7).

† **Open question:** GPIO 19 is configured as an *output* (GPFSEL1 all
outputs) but `arbTest` reads it as the CPLD's BGACK. Either the CPLD
drives it during status reads with the Pi's driver disabled, or this is
work-in-progress instrumentation. Not resolvable from this repo alone —
check against the CPLD bitstream. See §8.

---

## 3. Transaction protocol

### 3.1 Command/register encoding

The two command bits (GPIO 2/3) select which "register" a transaction
addresses; the 16-bit bus (GPIO 8–23) carries the payload:

```
GPIO2/3   register      use
00        REG_DATA      data for writes / data returned on reads
01        REG_ADDR_LO   A0–A15
10        REG_ADDR_HI   A16–A23 | FC | transfer type
11        REG_STATUS    status word read/write (HALT/RESET/INIT/…)
```

### 3.2 24-bit address + FC + type packing (the REG_ADDR_HI phase)

The Atari uses 24 address bits; the 32-bit GPIO group is reused. The CPLD
receives, in the **ADDR_HI** phase, on GPIO 8–23:

```
GPIO 8–15   A16–A23        (addr >> 16)
GPIO 16     type bit 0     = byte (1) / word (0)
GPIO 17     type bit 1     = read (1) / write (0)
GPIO 18     (unused)
GPIO 19–20  (unused)
GPIO 21–23  FC0–FC2        function codes (fc << 13, shifted up by 8)
```

The four `io_type` constants in `ps_protocol.h` are the pre-packed values
(`WRITE_BYTE 0x0100, WRITE_WORD 0x0000, READ_BYTE 0x0300, READ_WORD 0x0200`)
which land on GPIO 16/17 after the `<< 8`.

Byte/word alignment is handled by the Pi: `ps_write_8`/`ps_read_8` shift
the byte into the high or low half according to `addr & 1` (68000 UDS/LDS
convention); the CPLD derives the strobes. 32-bit accesses are two word
transactions (hi word at `addr`, lo word at `addr+2`) taken under **one**
bus lock so nothing interleaves (§3.5).

### 3.3 Write transaction (`ps_write_txn`)

```
1. drive D0–D15 on GPIO 8–23, select REG_DATA, pulse WR
2. drive A0–A15,  select REG_ADDR_LO, pulse WR
3. drive A16–A23|FC|type, select REG_ADDR_HI, pulse WR
4. poll GPLEV0 until PI_TXN_IN_PROGRESS (bit 0) clears
   → the completing sample also carries the status bits:
     BERR (bit 7) is extracted from it, no separate status read
```

The WR strobe (`txn_go` / `txn_pulse`) is deliberately doubled: set WR,
set WR again, clear WR, clear WR again, then `TXN_END`
(`0x00FFFF1E` = clear GPIO 1–4 and 8–23). The double writes are for
GPIO write-propagation delay; the CPLD latches on the WR edge.

### 3.4 Read transaction (`ps_read_txn`)

```
1. drive A0–A15,  select REG_ADDR_LO, pulse WR
2. drive A16–A23|FC|type, select REG_ADDR_HI, pulse WR
3. select REG_DATA, raise PIN_RD
4. poll GPLEV0 until TXN_IN_PROGRESS clears   ← data is *presented* here
5. RE-SAMPLE GPLEV0 PISTORM_RD_SETTLE times   ← the settle fix (below)
6. extract data = status >> 8, BERR from bit 7
7. drop RD / teardown (TXN_END)
```

**The read-settle saga (why step 5 exists).** Data on the completing GPLEV0
sample was originally taken as gospel. It is a nanosecond race: TXN
(GPIO0, driven by the CPLD) and data (GPIO8–23, driven through the '374
latches) arrive via different drivers, flight paths and pads, and the
sampling loop has no clock edge to synchronise against like a real 68000
would. Measured (2026-08-17): 28–69 of 512 bytes differing between two
reads of *unchanging* RAM on bad boots, zero on good ones; whole sectors
returning byte-swapped echoes of the Pi's own address phases. Each extra
GPLEV0 read costs ~50–100 ns of settle; the sweep found 1 insufficient,
**4 clean** (default; `PISTORM_RD_SETTLE` tunes, clamped 0–8).

### 3.5 Bus ownership: `ps_txn_lock` and `ps_bus_active`

All transactions (CPU thread) and all status/ipl reads (ipl_task thread,
diagnostics) serialize on a spinlock (`ps_txn_lock`). While a thread owns
the bus it may be **driving** the GPIO bank, so GPLEV0 samples taken by
ipl_task inside that window can contain *data pattern bits in the IPL
field* — phantom interrupt levels that scale with bus traffic (field case:
IDE streaming produced ~13 phantom level-4/s and ~90 phantom level-2/s).
`ps_bus_active` is set for exactly the drive window; ipl_task brackets its
sample with it (checked before *and* after the read) and discards samples
taken while set. The persistence filter (§7.3) covers the residual race.

### 3.6 BERR handling

`CHECK_BERR(x) = !((x & 0x80) >> 7)` — GPIO 7, active low. Every
read/write path records `g_buserr` and `g_buserr_addr` on error. The JIT
side hooks this with `jit_signal_buserr()` (SPCFLAG_BRK + longjmp out of
translated code; `g_buserr_addr` is used to raise `Exception(2)`).

---

## 4. Status register protocol (`REG_STATUS`)

`ps_read_status_reg()` returns the **full 32-bit GPLEV0** raw value (after
a `REG_STATUS + PIN_RD` strobe) — this is how the Pi reads the CPLD's
extra inputs, including the raw BGACK observation bit 19 (§8). Writes go
through `ps_write_status_reg()` and carry control bits:

| Bit | Macro | Meaning |
|---|---|---|
| 0 | `STATUS_BIT_HALT` | pulse: assert HALT for 1 ms |
| 1 | `STATUS_BIT_RESET` | pulse: assert RESET (see caveat below) |
| 2 | `STATUS_BIT_INIT` | pulse: reset the CPLD state machine |
| 2 | `LATCH_TYPE` | latch type select (LS373 vs LS374) — set with the 'P' key handshake |

Notes:

- `ps_reset_state_machine()` = INIT pulse (1 ms) then clear — the
  documented first step after power-up.
- `ps_pulse_halt()` = HALT for 1 ms.
- `ps_pulse_reset()` asserts RESET for **250 ms** (matching the comment and
  the SMI twin in `gpio/smi/ps_protocol_smi.c`, which always used
  `usleep(250000)`), then 1 ms settle. The earlier `usleep(15)` (15 µs)
  was a bug — fixed, see §10.1. Safe by construction: RESET is
  level-sensitive on every ST chip (GLUE/MFP/Shifter/DMA/FDC/PSG), there
  is no maximum width, and the ST's own power-on RC reset runs 100–250 ms.
  The only cost is the CPU thread pausing ~250 ms while the pulse is
  asserted (RESET-instruction path) — the bus lock is not held during the
  sleep (per-call acquire/release in `ps_write_status_reg`), so `ipl_task`
  keeps sampling and any interrupts simply latch until the CPU resumes.
- Latch type: `ps_write_latchtype()` reads bits 14/15 of the status word
  to see if the type is already set; if not, it writes `'P' << 3 |
  latchtype << 2` — the firmware latch-type handshake.
- Firmware revision: 11 bits of the status word, release/alpha/beta
  encoded in bits 10–8, chip type in bit 8 (EPM570 vs EPM240).
- Status writes do **not** produce a TXN_IN_PROGRESS handshake — the Pi
  waits for idle, writes, and moves on.

`ps_diag()` exercises the CPLD's loopback routing: GPIO6→GPIO0,
GPIO2→GPIO1, GPIO3→GPIO7 (inverted) — a quick wiring sanity check.

---

## 5. Memory dispatch: natmem mirror vs the bus

`pistorm_natmem.cpp` installs UAE bank handlers. Anything the Pi can serve
from its own memory is **never put on the Atari bus**:

- **ST-RAM / ROM / TT-RAM** → direct access to the mirror
  (`natmem_offset`), with per-bank `jit_read_flag`/`jit_write_flag` forcing
  I/O and ST-RAM *writes* back onto the handler (bus) path. `comptrust`
  stays 0 so the JIT always honours the flags.
- **Everything else** (MMIO: MFP `$FFFA00`, GLUE/Shifter `$FF8200`, DMA/FDC
  `$FF8600`, PSG, ACIA, IDE, blitter page, …) → `ps_read_*`/`ps_write_*`
  GPIO transactions, through `dm_*`/`io_*`/`sr_*` handler families, each
  ending in `pistorm_buserr()` which folds the transaction's BERR into the
  guest bus-error machinery.

Function codes are driven per access from the 68k core state:

```c
fc_data() → fc = regs.s ? 0x5 : 0x1;   /* supervisor/user data */
fc_prog() → fc = regs.s ? 0x6 : 0x2;   /* supervisor/user program */
```

The `fc` global is set by the CPU thread immediately before each
transaction; the IACK path uses FC 7 explicitly (§7.5).

**ST-RAM physical sizing** (`stram_alias_init`): the emulator probes the
*actual* DRAM on the board by driving a valid function code (`fc = 0x5` —
FC 0 is reserved and the GLUE will not decode RAM cycles for it) and
writing an arithmetic pattern to the memory-config register `$FF8001`,
then checking for ST-vs-STE alias fold offsets (`+0x208`/`+0x40008` for
128K, `+0x408`/`+0x80008` for 512K, `+0x8` intact for 2M). This is exactly
the dance TOS itself does, and it exists because a guest bank must ride a
physical chip at least its size.

The README's "Direct access needs developing" TODO is the next step of
this: `ps_latch_addr_st()` exists but is currently unused — it latches the
address directly onto the bus (373/374 style) so the Atari's address
decode runs in hardware instead of the Pi having to replay every cycle.

---

## 6. The thread model

| Thread | Pinned | Role |
|---|---|---|
| CPU thread (JIT) | cores 0/1 (normal) | runs translated 68k code; issues bus transactions |
| `ipl_task` | core 3 (isolated) | samples IPL, VBL/FDD/stbox housekeeping, stalls accounting |
| render / host | cores 0/1 | DRM, SDL, file I/O — never on core 3 |

`ipl_task`'s admission rule (from STBOX-DESIGN.md): **no syscalls, no
locks, bounded sub-microsecond work**. All its timing uses the ARM generic
timer (`CNTVCT_EL0` via `wait_ns`) — `clock_gettime` was ~40% of the
poll-loop profile.

Shared state between CPU thread and ipl_task: `g_ipl` (sampled level),
`g_irq` (latched interrupt level), `g_irq_mask`, `g_buserr`/
`g_buserr_addr`, `ps_bus_active`, `pistorm_iack_vector`, plus the
diagnostic counters. False sharing between `g_ipl` and `g_irq`/`g_irq_mask`
was a measured tax; `g_ipl` is only written on change.

---

## 7. Interrupts (the thorny half)

### 7.1 Where the levels come from

- **Level 2 (HBL)** and **level 4 (VBL)** — driven by the GLUE.
- **Level 6** — driven by the MFP (vector 0x40–0x4F), for Timer A/B/C/D,
  GPIP, ACIA and DMA sound.
- Level 7 (NMI) does not exist on the ST; levels 1/3/5 do not occur on a
  stock machine.

The 68000's IPL lines are active-low, 3-bit; the CPLD **[inferred]**
inverts/encodes them into the two active-high binary bits on GPIO 5/6 that
the Pi reads:

```c
ipl = (status & 0x60) >> 4;   /* GPIO6 → 4, GPIO5 → 2  ⇒  0, 2, 4, 6 */
```

(The `USING_PI_CLK` pinout additionally exposes IPL0 as `PI_IPL_ZERO`,
which would let odd levels 1/3/5/7 be distinguished — unused here.)

### 7.2 The sampler (`ipl_task`, emulator.c)

The loop, every pass:

1. Housekeeping slot (FDD 50 Hz tick, `_MCH` cookie watcher, STBOX
   slice) — checked *before* the bus-busy branch so a busy stretch cannot
   stall them.
2. **Bus-busy bracketing:** if `ps_bus_active`, or it becomes set between
   the check and the read, the sample is discarded — driven bus pins
   corrupt the IPL field (see §3.5).
3. Wait out an in-flight transaction (`status & 0x01` → 250 ns yield).
4. Sample `ipl = (status & 0x60) >> 4`.

### 7.3 The synchronizer

A changed level is only honoured after it has **held stable for a
persistence window** (default 2 µs, `PISTORM_IPL_CONFIRM_NS`). Why: the ST
drives IPL from two chips (GLUE + MFP) whose lines do not switch
atomically — a level-6 edge passes through the phantom codes `%100` (4)
and `%010` (2). Field-measured: ~75 phantom level-4/s and ~150 phantom
level-2/s latched on a 60 Hz screen riding on ~350 level-6/s; Bad Apple's
VBL-gated player ran ~12% fast. Two consecutive agreeing samples (the
68000's own rule) do **not** filter these — the CPLD latches a phantom at
its own sample moment and holds it, so back-to-back reads agree on the
same held phantom. Stability across *time* is required. Real sources lose
nothing: VBL/HBL/MFP are level-sensitive and hold until serviced.

`PISTORM_IPL_RAW=1` restores single-sample latching (A/B probe).

### 7.4 Latching a level → the CPU

When a confirmed level changes, `g_ipl` is updated (with episode counters
`pistorm_ipl_ep2/4/6`), and if the level is unmasked (`> g_irq_mask`) and
new (`!= av_held`), `g_irq` is latched and `jit_request_cpu_exit()` fires
— SPCFLAG_BRK plus `pissoff = -1` — which breaks the compiled block at
the next boundary so the CPU thread can take the exception. This is the
**only** interrupt path; the UAE `SPCFLAG_INT`/`SPCFLAG_DOINT` machinery is
deliberately not used (jit_glue.cpp §4).

### 7.5 Autovector edge semantics (the GLUE problem)

On real hardware, the CPU's interrupt-acknowledge cycle (a 6800-style
VPA/VMA handshake for autovectors) clears the GLUE's pending state — one
exception per VBL/HBL. The Pi's IACK cycle **evidently does not complete
that handshake**: the line stays asserted for the whole blanking interval,
and every RTE inside that window re-met the still-asserted line and
delivered the *same VBL again* (field-measured 75–84 level-4/s on a screen
that can only make 50/60/71.4). The software fix in ipl_task:

- `av_held` — remember the level latched this assertion episode; re-arm
  only when the line visibly leaves the level. One latch per episode.
- Level-4 refractory (default 5 ms, `PISTORM_VBL_REFRACT_NS`) — covers the
  4→6→4 interleave where an MFP interrupt mid-blank briefly masks the line
  and would otherwise re-arm the same VBL. (Level 2 gets none: HBL's
  period is 64 µs. Level 6 is untouched: the MFP vector handshake works.)

### 7.6 Delivery: the IACK read

`do_interrupt()` (newcpu.cpp) calls `intlev_ack(nr)` **before** building
the exception frame:

```c
vector = ps_read_8_fc(0x00FFFFF1u | (nr << 1), 7, &berr);
```

That is a real 68000 interrupt-acknowledge cycle on the bus: FC = 7,
address bits A3–A1 = level, byte cycle. The CPLD **[inferred]** decodes
this into the VPA handshake for the GLUE (levels 2/4 autovector) or the
MFP (level 6, returns 0x40–0x4F). The returned byte lands in
`pistorm_iack_vector`, and `effective_interrupt_vector()` (newcpu.cpp)
decides:

| `pistorm_iack_vector` | Result |
|---|---|
| `0xFFFE` (IACK bus error) | autovector fallback `24+nr` — then see §7.7 |
| `0xFFFF` (sentinel) | autovector fallback `24+nr` |
| level 6, 0x40–0x4F | the MFP vector (vector-table lookup at `vbr + 4*vec`) |
| anything else | autovector fallback |

The `Exception_normal()` patch is the one-line core change that makes the
autovector path honour the hardware vector: `eff_vec = interrupt ?
pistorm_iack_vector : vector_nr`.

### 7.7 The MFP no-ack recovery ladder

A level-6 IACK can fail (BERR → `0xFFFE`) when the CPLD cannot complete
the MFP's VPA vector handshake. `do_interrupt()` escalates:

1. **Retry once** (up to 2 attempts, `noack_retries`).
2. **Candidate vector** — if the last MFP vector seen (0x40–0x4F) is
   remembered, use it.
3. **ISRB snoop** — read the guest's MFP interrupt-in-service register
   memory state (`atari_recover_mfp_vector_from_isrb()`, respects the
   guest's IERA/IMRA masks) and use the highest pending bit.
4. **Drop** — if the live IPL line still reads 6 (or is unreadable), keep
   the latch (`clear_irq_latch = false`) and skip the exception, letting
   the next poll re-attempt; otherwise take the exception with the
   autovector and clear the latch.

### 7.8 Virtual level-6 sources (no bus involved)

The real MFP cannot raise an interrupt for things the Pi injects:

- **USB/Bluetooth keyboard** → IKBD bytes (MFP GPIP4): `ipl_task`
  synthesises level 6 when `kbd_usb_irq_wanted()` and the guest mask
  allows; `intlev_ack()` supplies vector `0x46` directly — a bus IACK
  would BERR because the real MFP has nothing to acknowledge. A racing
  *real* level-6 wins the bus IACK first; the injected byte stays pending
  and re-raises after the guest's RTE.
- **STE DMA sound frame interrupts** (Timer A event count / GPIP7):
  identical synthesis, checked *before* the keyboard to match real MFP
  channel priority; vector comes from the snooped MFP state.

### 7.9 HBL suppression (do-nothing handler fast-path)

Under MiNT the level-2 HBL fires at line rate (~15.7 kHz) into a bare-RTE
handler — each delivery is a full exception+RTE round trip, several % of
CPU at idle. `do_interrupt()` inspects the guest's level-2 vector:

- **kind 1**: `4E73` (bare RTE) — skip the exception entirely (but only
  when the CPU is not STOPped; a stopped CPU must be woken by the
  exception).
- **kind 2**: EmuTOS `int_hbl` (signature `3F00 302F 0002 0240 0700 6606`
  at `$E007AC`) — the whole observable effect is "if interrupted IPL was
  0, raise it to 3", which is implemented by setting `regs.intmask = 3`
  without the exception round trip.

The IACK still runs first either way — the GLUE handshake is preserved.

### 7.10 Latency model: `pissoff`

The JIT runs compiled blocks in chains; `pissoff` is the compiled-chain
budget in cycle-units (default `1024 × CYCLE_UNIT`, tunable via
`PISTORM_PISSOFF`). Bigger = longer runs = fewer C round-trips (CoreMark
540→734 at 1024) at the cost of a wider *fallback* interrupt window.
Real IPLs still break the chain immediately via SPCFLAG_BRK, so MFP
latency is dominated by the break, not the budget. The failure signature
of a break that got delayed is **keyboard beep / erratic mouse** — back
the multiplier off.

---

## 8. Bus arbitration (the other thorny half)

### 8.1 The 68000 contract

Other bus masters request the bus from the CPU:

```
master:  assert BR (bus request)
CPU:     assert BG (bus grant) between bus cycles
master:  assert BGACK (bus acknowledge), drop BR
CPU:     float AS/UDS/LDS/R/W/address/data — bus is now the master's
master:  ...cycles... drop BGACK
CPU:     negate BG, resume driving
```

On the Atari ST the masters are the **BLiTTER** (`$FF8A00`) and the
**DMA controller** (FDC/ACSI/Shifter DMA). A PiStorm that cannot arbitrate
can never let those devices own the bus.

### 8.2 Current status: **not working** (this is the headline)

README known issue, verbatim:

> Bus Arbitration is not working so Blitter, FDD, external ACSI bus
> devices will not work at the moment.

Concretely, the CPLD bitstream does not implement the arbitration state
machine, so the Blitter/DMA are never granted the bus. The config file
(`configs/master.cfg`) encodes the workaround:

```
blitter            emulated ST BLiTTER (default) — software blit over the RAM
                   mirror, real chip untouched; works with or without a
                   blitter fitted, no CPLD bus arbitration needed
blitter real       pass register access through to the real chip
                   (requires working bus arbitration in the CPLD)
blitter disabled   hide FF8A00-FF8BFF: TOS detects no blitter and uses
                   software rendering
```

Same story in `pistorm_natmem.cpp` ("real" mode needs CPLD arbitration,
"which the current bitstream does not have"). FDD emulation runs the WD1772
in software (`platforms/atari/fdd/`), so floppy images work without DMA;
real FDD/ACSI DMA does not.

### 8.3 What the Pi side would need to do

From the code that exists, the intended division of labour:

- **Observe:** `arbTest` polls the status register and tests
  `ARB_STATUS_BUSY_RAW` = raw GPIO 19 = **BGACK** (ataritest comment:
  "raw19=BGACK"). The CPLD is expected to expose BGACK on that line.
  `PISTORM_ARB_WAIT=1` forces slow diagnostic pre-cycle polling.
- **Float:** on BGACK, the Pi must stop driving the bus (tristate the
  GPIO bank / stop issuing transactions). This is the missing piece on the
  Pi side — nothing in `ps_protocol.c` currently releases the bus; the
  protocol is strictly master-only (Pi initiates every transaction).
- **Resume:** on BGACK deassert, re-assert BG and continue.

`arbTest` (ataritest `--arbtest`) is the measurement rig: for N seconds it
samples the BGACK status bit (arb-seen rate, transitions), can optionally
hammer RAM (write16/read16 with BERR accounting), and can trigger *real*
blitter operations (`--source=blitter --op=fill|copy`) while watching the
blitter's busy/complete behaviour — the tool that characterises what a
given CPLD bitstream actually does, and the natural starting point for
developing the Pi-side arbitration logic.

### 8.4 What's missing (the honest list)

1. CPLD arbitration state machine (BR/BG/BGACK sequencing) — external,
   not in this repo.
2. Pi-side: BGACK observation integrated into the CPU loop (the raw19
   read exists only in ataritest today), bus-release/floating logic, and
   BG assertion.
3. GPIO 19 direction question from §2 — raw19 is only observable if the
   CPLD can drive it (see open question).
4. Read/transaction semantics for the granted period: the CPLD would need
   to hold the Pi's transactions off while the external master owns the
   bus (or BERR them).

---

## 9. Environment knobs (all verified in code)

| Var | Default | Effect |
|---|---|---|
| `PISTORM_RD_SETTLE` | 4 | extra GPLEV0 re-samples after the read handshake (0–8) |
| `PISTORM_IPL_RAW` | off | single-sample IPL latching (bypasses the 2 µs synchronizer) |
| `PISTORM_IPL_CONFIRM_NS` | 2000 | IPL persistence window (0 = two-sample minimum) |
| `PISTORM_VBL_REFRACT_NS` | 5000000 | level-4 VBL re-arm refractory |
| `PISTORM_PISSOFF` | 1024 | compiled-chain budget multiplier (1–65536) |
| `PISTORM_ARB_WAIT` | off | slow diagnostic pre-cycle polling in arbTest |
| `PISTORM_NATMEM_LIMIT` | `0x09000000` | guest-address bound for natmem fault attribution |

Diagnostic counters maintained for the 1 Hz summaries:
`pistorm_ipl_lat2/4/6` (levels presented to the CPU),
`pistorm_ipl_ep2/4/6` (assertion episodes — the difference is the
redelivery question), `pistorm_mfp_iack_counts[16]`,
`pistorm_mfp_last_iack_vector`, `pistorm_iack_vector`.

---

## 10. Known issues & open questions (fresh eyes)

0. **MINIMAL build seam** — `make MINIMAL=1` strips every platform extra
   (FDD, IDE, DMA sound, YM2149, ET4000, video/AV record, USB keyboard,
   network, STBOX, Musashi, NatFeats) via a stub layer
   (`platforms/atari/minimal_stubs.cpp`) + Makefile file-list exclusion;
   the core sources need no #ifdefs. Cross-verified (2026-08-30,
   aarch64-none-linux-gnu 13.3): compiles, links against only
   libstdc++/libm/libgcc_s/libc, and the binary executes under
   qemu-aarch64. Pair with `configs/core.cfg`.

   **ST-RAM shadow control** (config `stram_nomirror`): by default the
   mirror serves READS while every WRITE goes through to the real bus
   (`stram_cache` off is the default). `stram_nomirror` routes READS AND
   WRITES over the real bus (the jan-2025 "everything real" behaviour) —
   the JIT's direct-access path is disabled via `jit_read_flag = S_READ`
   + clearing `ABFLAG_DIRECTACCESS` on the ST-RAM bank. The remaining
   coherence hole (an external master WRITING real DRAM leaves the mirror
   stale) is the future CPLD-BGACK invalidation work.
1. **~~Reset pulse width~~ — RESOLVED (bug, fixed).** `ps_pulse_reset()`
   slept 15 µs while its comment (and the SMI twin implementation in
   `gpio/smi/ps_protocol_smi.c`) say 250 ms. Corrected to
   `usleep(250000)` + 1 ms settle. Safe per §4. The remaining open
   questions:
2. **raw19/BGACK pin direction** — GPIO 19 is an output in
   `GPFSEL1_OUTPUT`; reading it as BGACK implies the CPLD can drive it
   (contention?) or a future board change. The shipped SVFs (see §1.1)
   contain no pin-assignment data (that lives in the private Quartus
   project), so this stays a hardware question: probe GPIO 19 with
   `raspi-gpio get 19` while a blitter/DMA runs and see if it ever moves
   while configured as input.
3. **Autovector IACK handshake** — the GLUE's pending state is not
   cleared by the Pi's IACK (hence the episode/refractory logic in
   software). A CPLD that completed the VPA/VMA handshake properly would
   remove a whole class of workarounds.
4. **Bus arbitration** — see §8. The biggest single feature gap, and the
   reason real blitters, FDC DMA and ACSI devices are off.
5. **SMI protocol** (`gpio/smi/`) exists in-tree but the Atari build does
   not use it — don't be confused by the two protocol implementations.
6. **Which bitstream is shipped?** The in-tree SVFs are all
   "dev"-flavoured builds; nothing in the repo says which one a user
   should flash. `dev.svf` (Aug 15) is the newest by build time, but a
   "works"-named older image being flashed by users would explain
   behaviour differences between setups.
