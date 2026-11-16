# 03 — Proposed Repository Structure

```
G3MacPi/
├── README.md                    Project overview, status, quick start
├── LICENSE                      GPL-3.0-or-later (pending owner decision; Circle is GPL-3.0)
├── CMakeLists.txt               Host (Linux) build of core + tests
├── Makefile                     Bare-metal build via Circle (top-level wrapper)
├── .github/workflows/
│   ├── host-tests.yml           Build core + run all unit tests on Linux
│   └── pi-image.yml             Cross-compile kernel8-rpi4.img, publish SD-card zip artifact
├── docs/
│   ├── 01-feasibility.md
│   ├── 02-architecture.md
│   ├── 03-repository-structure.md
│   ├── 04-roadmap.md
│   ├── references.md            Every manual / datasheet / source used, and for what
│   ├── decisions/               Short architecture decision records (ADR-0001 ...)
│   ├── hardware/                One file per emulated device: registers, behaviour, sources
│   └── devlog/                  Dated notes on what was tested and how
├── core/                        Portable emulator — MUST NOT include Circle headers
│   ├── common/                  types, endian helpers, bit ops, logging facade, ring buffers
│   ├── cpu/ppc750/              state, decoder, interpreter, fpu, mmu, exceptions, disasm
│   ├── cpu/jit/                 (Milestone 8)
│   ├── mem/                     physical bus, RAM/ROM, MMIO dispatch, DMA helpers
│   ├── sched/                   virtual clock, event scheduler
│   ├── devices/
│   │   ├── pci/                 grackle, dec21154, config space
│   │   ├── macio/               paddington, pic, dbdma, via_cuda, adb, nvram, burgundy, scc, swim3
│   │   ├── ata/                 ATA/ATAPI core, paddington_ata, cmd646
│   │   ├── video/               simplefb, rage128 (later)
│   │   └── usb/                 ohci (later)
│   ├── storage/                 disk image formats, partition maps
│   ├── firmware/openfirm/       device tree, client interface, forth, bootinfo, hfs reader
│   ├── machine/yosemite/        B&W G3 wiring and configuration
│   └── debug/                   monitor, tracer, breakpoints
├── platform/
│   ├── include/platform/        IDisplay, IInput, IAudio, IBlockDevice, IFileSystem, IHostClock, ILog
│   ├── circle/                  Circle-backed implementations
│   └── host/                    Linux implementations (headless; optional SDL2)
├── app/
│   ├── circle/                  kernel.cpp, main.cpp, multicore, config menu, Makefile
│   └── host/                    g3macpi-host command-line runner
├── guest/                       PowerPC code we write ourselves
│   ├── testroms/                self-checking CPU/MMU test programs (assembly)
│   └── ndrv/                    our display driver for Mac OS (later)
├── tests/
│   ├── unit/                    per-module unit tests
│   ├── vectors/                 data-driven CPU test vectors
│   ├── reference/               reference device-tree dumps (text), no Apple binaries
│   └── integration/             boot tests (skip when user-supplied files absent)
├── sdcard/                      config.txt, g3macpi.ini template, layout README
├── tools/                       scripts: make-sdcard, vector generators, image tools
└── third_party/
    └── circle/                  git submodule, pinned release tag
```

## Rules enforced by the build

- `core/` and `platform/include/` compile with `-ffreestanding`-safe flags and a
  CI check that greps for any `#include <circle/...>` outside `platform/circle`
  and `app/circle`.
- No Apple-copyrighted binaries anywhere in the repository (`.gitignore` covers
  `*.rom`, `Mac OS ROM`, `*.img`, `*.dsk`, `*.toast`, `*.hda` under user dirs).
- Each device directory has a matching `docs/hardware/*.md` with sources.

## SD card layout (FAT32 boot partition)

```
/start4.elf, /fixup4.dat, /bcm2711-rpi-400.dtb   Raspberry Pi firmware (from raspberrypi/firmware)
/config.txt                                       arm_64bit=1, kernel=kernel8-rpi4.img
/kernel8-rpi4.img                                 our emulator
/g3macpi/g3macpi.ini                              configuration
/g3macpi/disks/*.img                              user-supplied disk images
/g3macpi/nvram.bin                                persisted NVRAM/PRAM
/g3macpi/logs/                                    log files
```
(Exact firmware filenames and `config.txt` keys to be verified in Milestone 1.)
