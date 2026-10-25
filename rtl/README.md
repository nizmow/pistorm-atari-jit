# Building and testing the Atari CPLD

Reconstructed Quartus Lite 20.1.1 projects build both checked-in 1.1b sources.
EPM240 targets **EPM240T100I5** (-40 to 100 C junction model); EPM570 targets
**EPM570T100C5**. These target selections are explicit, not auto-detection.
Check your chip/package/board before selecting a project. The classic DIP64
PiStorm pin map was checked against the official Rev B PCB, rather than old
interface signal names. See the [changelog](CHANGELOG.md) and
[validation appendix](TEST-SYSTEM.md).

## Build tasks

Install [mise](https://mise.jdx.dev/getting-started.html), then run `mise install`
from the repository root to install the pinned CMake 4.1.2 and Ninja 1.13.1.
Podman, Python 3 and Git are host prerequisites. `mise tasks` lists the CPLD tasks:

| Task | Result |
|---|---|
| `quartus:image` | Quartus/MAX II Podman image, using the local vendor files below |
| `build:cpld:epm240` | EPM240 program/verify SVFs and build manifest |
| `build:cpld:epm570` | EPM570 program/verify SVFs and build manifest |
| `build:cpld` | Both targets, sequentially |
| `test:cpld` | All 36 host regression tests |
| `test:cpld:arbiter` / `test:cpld:faults` | Focused arbiter/fault campaigns |

## Quartus Podman image and downloads

Download these Linux files from the [official Quartus Lite 20.1.1 page][quartus]
into `rtl/docker/` (ignored by Git):

| File | Purpose |
|---|---|
| `QuartusLiteSetup-20.1.1.720-linux.run` | Linux x86-64 Quartus Lite 20.1.1 Build 720 installer |
| `max-20.1.1.720.qdz` | Matching MAX II/MAX V device support, including EPM240/EPM570 |

Save exactly those two files alongside `rtl/docker/Containerfile`; they are
downloaded manually and are not fetched by mise or committed to Git. The Podman
build fetches the Ubuntu 20.04 base image and the apt dependencies listed in the
Containerfile. The unattended Quartus installer uses the co-located device pack,
then verifies the required MAX II parts. After reading and accepting the vendor
license, build and check the image from the repo root:

```sh
ACCEPT_INTEL_EULA=1 mise run quartus:image
podman run --rm localhost/pistorm-quartus:20.1.1
podman run --rm localhost/pistorm-quartus:20.1.1 quartus_sh -t /tmp/verify-devices.tcl
```

The equivalent image build without mise is
`podman build --build-arg ACCEPT_INTEL_EULA=1 --tag localhost/pistorm-quartus:20.1.1 rtl/docker`.
Build the image on an x86-64 host with Linux container support. The generated
SVFs can then be programmed from the target Pi.

The image and build wrapper preload `/lib/x86_64-linux-gnu/libudev.so.1` to
avoid Quartus's allocator conflict during device enumeration. Without it,
`quartus_map` can abort with `realloc(): invalid pointer`, followed by a generic
memory warning. Background and observed symptoms are recorded in the
[build-environment findings](TEST-SYSTEM.md#build-environment-findings).

## Compile and generate programming files

```sh
mise run build:cpld:epm240
# Or: mise run build:cpld:epm570 / mise run build:cpld
```

Without mise, use `bash rtl/tools/build.sh EPM240` (or `EPM570` / `all`).
The wrapper copies sources and
project settings into `build/cpld/EPM240/`, compiles there, and converts the POF
to separate `EPM240-program.svf` and `EPM240-verify.svf` files. It records source,
project and output hashes in `manifest.json` / `SHA256SUMS`. Generated outputs
do not overwrite the shipped SVFs. Override `CPLD_OUTPUT_DIR` or
`CPLD_QUARTUS_IMAGE` if needed. No flashing occurs in this command.

With native Quartus, run `quartus_sh --flow compile pistormsxb_devEPM240`
from `rtl/`, followed by `quartus_cpf -c -q 100KHz -g 3.3 -n p` (or `-n v`)
on `output_files_EPM240/pistormsxb_devEPM240.pof`. The wrapper is the easier
way to preserve exact build inputs and generate both files.

To audit the EPM240 fitted pin report, download the [classic Rev B archive][board]
and run from the repository root:

```sh
python3 rtl/tools/audit-epm240-pins.py \
  --board-archive /path/to/PiStormRevB_ascii.zip \
  --pin-report build/cpld/EPM240/output_files_EPM240/pistormsxb_devEPM240.pin
```

The checker pins the reviewed archive by SHA256 and derives all 56 signal
placements from PCB nets, including Pi header GPIOs and level-shifter pairs.
It checks directions, 3.3-V LVTTL, dedicated JTAG and rail-connected spare pins.
It checks the reference layout; it does not identify a physical PCB revision.

## Host regression tests

Run `mise run test:cpld -j 3` (or `bash tests/cpld/run.sh -j 3`). It bootstraps pinned Verilator 5.052 when
needed and runs 36 CTest entries. These are standalone host tests and require
no Pi, Atari or Quartus. See
[tests/cpld/README.md](../tests/cpld/README.md) for prerequisites, coverage and limits.

## Hardware use and remaining limits

Match the selected device, package, board wiring and latch type before
programming. Tested configurations and results are recorded in the
[validation appendix](TEST-SYSTEM.md). Existing shipped SVFs remain unchanged.
Firmware revision words
`$421`/`$521` are compatibility identities, not proof of the fixes: retain file hashes.

`M68K_CLK` is constrained to 124 ns (8.0645 MHz), covering the standard ~8 MHz
ST bus clock. Faster physical clocks require updated constraints. Five internal/interface
clocks and external input/output paths remain unconstrained in the EPM240 build.
The two original unsafe-latch warnings for `PI_BERR` and `RW_INT` remain.
Requests are captured by a command-write flip-flop. Simulation
does not model electrical propagation, setup/hold violations or metastability.
This is initial functional evidence, not full physical timing sign-off.

Keep a boot-tested rollback SVF and Pi SSH access. Stop the emulator service
and other GPIO users before programming; stopping a process alone is insufficient
when its service restarts it. From the working Pi installation, after copying
the matching files into `rtl/`:

```sh
sudo systemctl stop pistorm.service
sudo rtl/pistormflash -v -s rtl/EPM240-program.svf &&
sudo rtl/pistormflash -v -s rtl/EPM240-verify.svf
```

Use your installed service name. Program writes the configuration; verify
checks it through JTAG without rewriting it. After both succeed, power-cycle
before starting the same emulator/configuration. A verify file for a new
rebuild should not be expected to match a different installed payload. The
legacy flasher ignores requested JTAG frequency; its warning does not indicate
a verification failure. See the validation appendix for tested Pi configurations.
Preserve rollback files
separately rather than overwriting the working image.

[quartus]: https://www.altera.com/downloads/fpga-development-tools/quartus-prime-lite-edition-design-software-version-20-1-1-linux
[board]: https://github.com/captain-amygdala/pistorm/blob/main/PiStormRevB_ascii.zip
