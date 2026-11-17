# 04 — Development Roadmap

> **Update 2026-11-17:** ADR-0011 returned the project to this roadmap
> (the DingusPPC port of ADR-0010 was abandoned). M1 hardware checks are
> paused while the owner is busy; Milestone 2 (CPU) proceeds on Linux/CI.

Each milestone ends with a tagged Git commit and **acceptance tests that were
actually run**. Effort estimates are relative sizes (S/M/L/XL), not dates —
this is a long project and dates would be guesses.

The brief's milestones are kept, with three changes, each for a reason:
- **M0 added**: the host build and CI must exist before bare-metal work,
  because the Pi cannot compile while running the emulator and the iPad
  cannot compile at all.
- **Firmware split out (M5)**: it is the highest-risk component and needs its own
  milestone and exit criteria.
- **CPU before bare metal is allowed in parallel**: CPU work is tested on Linux,
  so it does not wait for the Pi.

| # | Milestone | Size | Exit criteria (all must be demonstrated) |
|---|---|---|---|
| **M0** ✅ (CI green; boots on Pi 400) | Repository + host build + CI | S | CMake host build; doctest-style test runner; GitHub Actions runs tests on every push; `aarch64-none-elf` cross-build of an empty Circle app produces `kernel8-rpi4.img` artifact |
| **M1** 🔶 in progress | Bare-metal foundation | S–M | On the Pi 400: boots from SD, HDMI startup screen, UART log, on-screen log, USB keyboard echo, log file written to SD, HDMI test tone; HTTP bootloader evaluated; `docs/devlog` entry with photos |
| **M2** 🔶 step 1 done (see docs/hardware/ppc750-cpu.md) | CPU: integer + branch + exceptions | L | All UISA integer/logical/rotate/compare/load/store/string/branch/CR/XER instructions; SPR access; `sc`, `rfi`, program/alignment exceptions; disassembler; ≥1 test per instruction form incl. edge cases (overflow, carry, record forms) |
| **M2b** ✅ (see docs/hardware/ppc750-cpu.md) | CPU: floating point | M–L | All 750 FP instructions, FPSCR semantics, NaN/denormal/rounding vectors pass |
| **M3** 🔶 done except Pi measurement (MMU, TLB, bus, scheduler) | Memory, MMU, execution loop | M | Physical bus; BAT + HTAB translation; soft TLB; DSI/ISI; self-checking test ROMs pass on host **and Pi**; first measured MIPS figure on the Pi (published in devlog) |
| **M4** 🔶 Grackle, DEC21154, PIC, VIA, Cuda, Paddington, ADB, DBDMA + assembled Yosemite machine; NVRAM, IDE next | Macintosh motherboard | L | Real B&W device-tree dump obtained; Grackle + PCI config; DEC21154; Paddington PIC + DBDMA; VIA/Cuda (RTC, PRAM, ADB); NVRAM; decrementer/timebase; each with `docs/hardware/` page + unit tests |
| **M5** | Open Firmware (HLE) | L–XL | Device tree matches reference dump (diff test); client interface; Forth engine runs the boot script of a user-supplied `Mac OS ROM`; Trampoline completes and jumps to NanoKernel (logged) |
| **M6** | Display, storage, input, audio | L | Paddington ATA + disk images (Apple Partition Map); simple framebuffer + our NDRV; ADB keyboard/mouse from Pi USB; Burgundy + DBDMA → HDMI audio |
| **M7** | Mac OS 9 boot | XL | Happy Mac → Mac OS 9.2.2 desktop on the Pi 400; Finder usable; known-issue list maintained |
| **M8** | Optimisation | XL | Profiling on Pi; memory fast paths; display dirty-rect conversion; PPC→AArch64 JIT with interpreter as reference; measured speed-ups recorded |
| **M9** | Fidelity extensions | XL | CMD646 Ultra ATA; USB OHCI + guest HID; ATI Rage 128 (for ATI drivers); optional LLE boot of a user-dumped B&W boot ROM |

## Critical-path risks and early spikes

| Risk | Spike (small experiment done early) |
|---|---|
| Firmware feasibility | In M2/M3, extract and read the boot script of a `Mac OS ROM` file (user-supplied, never committed) and list every Forth word it uses |
| Device tree accuracy | Ask the community / owners for a full B&W G3 OF dump early (M2 timeframe) |
| Interpreter speed | Micro-benchmark the dispatch loop on the Pi in M1/M2, before the CPU is large |
| Display driver | Prototype a minimal NDRV with the PowerPC toolchain in M4 |
| iPad workflow | Prove the CI-artifact → SD card (or HTTP bootloader) loop in M1 |

## First concrete steps after approval

1. M0-1: `LICENSE`, `.gitignore`, `CMakeLists.txt`, `core/common/endian.h` + tests.
2. M0-2: GitHub Actions host-test workflow.
3. M0-3: Circle submodule + empty kernel + cross-build workflow.
4. M1-1: startup screen + UART/screen logging on the Pi 400 (first hardware test by the owner).
