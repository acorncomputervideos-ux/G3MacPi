# 05 — Option B: porting DingusPPC to bare-metal Raspberry Pi 400

> **ABANDONED 2026-11-17 (ADR-0011).** The owner chose to return to the
> original, independently developed design. Kept as a record; the port
> work is recoverable from commits e8e44bf, c4816fe, 76edf95 and 24eb494.

Status (2026-11-16): **P1 done; P2 built, waiting for the Pi 400 run.**
DingusPPC is vendored in `third_party/dingusppc`, the Pi build uses
circle-stdlib, and the Pi kernel contains DingusPPC's CPU core and test suite.
Decision: ADR-0010.

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

### Linux baseline (owner permitted running DingusPPC code, 2026-11-16)
- CPU test suite: **7702 tested, 472 failed**; disassembler 397 tested, 0 failed.
  Same with gcc 13 and clang 18, with upstream's loguru or our replacement,
  with or without `-fno-strict-aliasing`.
  - 24 integer failures, all `divw`/`divwu` (+ `o`/`.` forms) edge cases.
    Note: the test program runs the CPU in **601 mode** (`is_601 = true`).
  - 448 FP failures: 312 wrong FPSCR flags only, 108 FPSCR + destination
    (cases with FP exceptions enabled, where hardware leaves the target
    register unchanged), 28 destination only. Known DingusPPC FPU gaps;
    improvement candidates.
- Benchmarks (x86-64 container): bench1 530 MiB/s; bench2 FPU
  15.3 million FP instructions/s (65 ns each) — FPU emulation is slow.

### P1 implementation notes
- `port/loguru/`: loguru replacement (LOG_F, VLOG_F, LOG_IF_F, ABORT_F,
  CHECK_F, named verbosities) forwarding to a sink: Circle `CLogger` on
  the Pi, stderr on Linux.
- One DingusPPC patch so far: `DppcRecursiveMutex` in `core/timermanager.h`
  (bare metal: `port/baremetal/dppc_mutex.h`, single-core, no locking).
- DingusPPC is compiled with `-fno-strict-aliasing` (its FPU code type-puns
  through pointers, which GCC warns about at -O2).
- Minimal CPU-test file set: `cpu/ppc/*.cpp`, `core/timermanager.cpp`,
  `devices/memctrl/memctrlbase.cpp`, plus the test sources.
- CI: `host-tests` runs DingusPPC's CPU suite against
  `tests/dingusppc/cpu_baseline.env` (fails on regressions).

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
