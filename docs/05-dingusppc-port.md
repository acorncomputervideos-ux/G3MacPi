# 05 — Option B: porting DingusPPC to bare-metal Raspberry Pi 400

Status: **feasibility test done (compile-level). No DingusPPC code is in this
repository yet.** Decision: ADR-0010.

## What DingusPPC is
- https://github.com/dingusdev/dingusppc — C++ low-level emulator of PowerPC
  Macintosh hardware (601/603/604/750 CPUs; Old World, early New World, Pippin).
- **Licence GPL-3.0** (checked LICENSE file) — compatible with Circle (GPL-3.0)
  and with this project (GPL-3.0-or-later).
- Upstream README: "highly unfinished"; most complete machines are the
  Power Mac 6100, 7500 and **Beige G3**. Interpreter only, no JIT.
- Has a **Blue & White G3 ("Yosemite", `pmg3nw`)** machine definition.
- Needs a **real Apple boot ROM** dump (1 MB at 0xFFF00000 for the B&W).
- Tested commit: `d0fb711842f6e9851394ea036b049668b3119d52` (2026-11-10),
  ~5 MB of source excluding git history.

## Feasibility test (2026-11-16)

### Host dependencies (by inspection)
| Dependency | Where | Bare-metal plan |
|---|---|---|
| SDL2 | `core/hostevents_sdl.cpp`, `devices/video/display_sdl.cpp`, `main_sdl.cpp`, ESCC host hooks | Replace with Circle back-ends (our input queue, Circle framebuffer) |
| cubeb (audio) | `devices/sound/soundserver_cubeb.cpp` | Replace with Circle HDMI sound |
| loguru (logging) | everywhere (`LOG_F`, `ABORT_F`) | Small adapter onto Circle's `CLogger` |
| termios / stdin | `devices/serial/chario.cpp` (host serial backend) | UART backend via Circle |
| Interactive debugger | `debugger/debugger.cpp` | Later; replace with our monitor |
| C++20 + standard library (vector, map, function, chrono, cfenv, iostream, filesystem) | everywhere | **circle-stdlib** (newlib + libstdc++/libc++ on Circle, GPL-3.0) |
| `std::recursive_mutex` | `core/timermanager.h` | Shim (emulation runs on one core) or Circle spin lock |
| Threads | none in emulation code | — |
| `setjmp`/`longjmp` (CPU exceptions) | `cpu/ppc` | Works with newlib |

### Cross-compile result
Every emulation source file (cpu, core, devices, machines, debugger, utils;
excluding the SDL/cubeb files) was compiled with the project's toolchain,
Arm GNU 15.2.rel1 `aarch64-none-elf-g++ -std=gnu++20 -O2 -mcpu=cortex-a72`,
against the toolchain's own newlib/libstdc++ headers:

| Attempt | Result |
|---|---|
| As-is, `-std=c++20` | 42 / 94 compile |
| `-std=gnu++20` + test-only `recursive_mutex` stand-in, loguru excluded | 74 / 93 |
| + empty stand-in for `sys/termios.h` | **91 / 93** |

The two remaining failures are host-terminal code that will be replaced
(`devices/serial/chario.cpp`, `debugger/debugger.cpp`). Everything the
Blue & White needs compiles: PPC CPU/MMU/FPU, `machineyosemite.cpp`, MPC106
(Grackle), mac-io, CMD646, ATA/ATAPI, VIA-Cuda/ADB, DBDMA, Burgundy, ATI Rage.

### Not yet done (and why)
- **Running DingusPPC's own test suite and benchmarks on Linux**: built
  successfully, but executing third-party downloaded code in the Claude
  session was blocked by the session's safety policy pending the owner's
  permission. Options: owner grants permission, or run them in GitHub
  Actions.
- **Linking** against circle-stdlib and **running on the Pi**: next step.

## Revised plan (replaces milestones M2–M7 in 04-roadmap.md)

| # | Step | Exit criteria |
|---|---|---|
| P1 | Vendor DingusPPC at a pinned commit into `third_party/dingusppc` (LICENSE/CREDITS kept, upstream commit recorded, our changes tracked as normal commits). Switch the Pi build to circle-stdlib. | Pi image builds with DingusPPC CPU linked in |
| P2 | **CPU validation**: run DingusPPC's 7,674 CPU test vectors (`ppcinttests.csv`, `ppcfloattests.csv`) in CI **and on the Pi 400** (vectors read from SD card) | All vectors pass on Pi, or failures listed and explained |
| P3 | **First benchmark**: DingusPPC `bench1/bench2` on the Pi 400 | Measured guest MIPS published in devlog |
| P4 | Host glue: loguru→Circle logger, timer manager→Circle timer, file I/O (ROM, disk images) via newlib/FatFs on SD, display→Circle framebuffer, input→our HID queue → DingusPPC ADB, UART serial | Machine starts and executes ROM code on the Pi |
| P5 | **First boot target: Beige G3** (the most complete DingusPPC machine; needs a Beige G3 ROM) — proves the port | Mac OS boots to desktop on the Pi 400 |
| P6 | **Blue & White G3**: needs a B&W boot ROM dump; fix gaps (CMD646 hard-disk boot, etc.) | Mac OS 9 boots on the emulated B&W |
| P7 | Performance: profiling, memory fast paths, multi-core split (display/audio off the emulation core), then PPC→AArch64 JIT | Measured speed-ups |
| P8 | Optional: our own HLE Open Firmware (original plan) so the B&W boots without a hardware ROM dump | — |

Improvements that are generally useful should be offered back upstream to
DingusPPC; bare-metal specifics stay here.

## What stays from our own work
Circle application, CI, SD-card packaging, logging to SD, HID keyboard
tracker and SPSC input queue, and the documentation/test discipline. The
`core/common` helpers remain for our own glue code.
