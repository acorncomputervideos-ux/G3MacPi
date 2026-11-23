# 02 — Software Architecture

Status: **proposed, awaiting approval.** Nothing here is implemented yet.

## 1. Design principles

1. **Portable core.** Everything that emulates the Macintosh (CPU, memory, devices,
   firmware) is plain C++17 with **no Circle headers**. It builds on Linux for
   tests and on the Pi inside Circle. This is what makes the project testable
   from an iPad via CI.
2. **Thin platform layer.** A handful of small interfaces (display, input,
   audio, block storage, files, clock, log). One implementation uses Circle;
   one uses plain Linux (headless, for tests; optional SDL2 for desktop debugging).
3. **Firmware is separate from hardware and CPU.** The CPU exposes a generic
   "host call" hook; the firmware module registers on it. Devices never
   know firmware exists.
4. **One guest thread, deterministic.** The CPU and all guest devices run on a
   single host core, scheduled by guest time. Same inputs ⇒ same execution.
   That makes bugs reproducible and tests reliable.
5. **Bare-metal-friendly C++.** No exceptions, no RTTI, no allocation in hot paths,
   fixed-width types, explicit endianness helpers.
6. **Every device documented before it is written** (`docs/hardware/<device>.md`
   with sources), per the brief's rules.

## 2. Layer diagram

```
+------------------------------------------------------------------+
| app/circle  (Circle kernel: boot, config menu, core assignment)  |
| app/host    (Linux runner: headless tests, optional SDL window)  |
+-------------------------------+----------------------------------+
| platform/ interfaces          | platform/circle | platform/host  |
|  IDisplay IInput IAudio       |  (Circle impls) |  (Linux impls) |
|  IBlockDevice IFileSystem     |                 |                |
|  IHostClock ILog              |                 |                |
+-------------------------------+----------------------------------+
| core/  — NO platform dependencies                                |
|                                                                  |
|  machine/yosemite   (wires the B&W G3 together, owns everything) |
|      |                                                           |
|  +---+-------------+-----------------+----------------------+    |
|  | cpu/ppc750      | mem/            | firmware/openfirm     |   |
|  |  state, decode, |  PhysicalBus    |  device tree builder  |   |
|  |  interpreter,   |  RAM, ROM,      |  client interface     |   |
|  |  FPU, MMU(BAT/  |  MMIO dispatch  |  Forth engine         |   |
|  |  HTAB, soft TLB)|  DMA helpers    |  bootinfo loader      |   |
|  |  exceptions     |                 |  NVRAM layout         |   |
|  |  (later: jit/)  |                 |                       |   |
|  +-----------------+-----------------+----------------------+    |
|  devices/                                                        |
|    pci/  grackle, dec21154, pci config space, BAR handling       |
|    macio/ paddington: pic, dbdma, ata, scc(stub), swim3(stub),   |
|           via + cuda (adb, rtc, pram, power), burgundy           |
|    ata/  ATA/ATAPI command layer shared by Paddington & CMD646   |
|    video/ simplefb (own PCI device + own NDRV), rage128 (later)  |
|    usb/  ohci + guest HID (later)                                |
|  storage/  disk images (raw, Apple Partition Map, later others)  |
|  sched/    guest-time event scheduler, virtual clock             |
|  debug/    disassembler, tracer, breakpoints, monitor commands   |
+------------------------------------------------------------------+
```

## 3. Key components

### 3.1 CPU (`core/cpu/ppc750`)
- `CpuState`: GPRs, FPRs (stored as `uint64_t` bit patterns), CR, XER, LR, CTR,
  FPSCR, MSR, SRs, BATs, SPRs, reservation, pending-exception flags.
- **Decoder:** table indexed by primary opcode (6 bits) then extended opcode;
  produces a handler pointer. Later a decoded-block cache keyed by physical
  page, invalidated on writes to code pages and `icbi`.
- **Interpreter:** `run(budget)` executes until the cycle budget is used or an
  exception/interrupt check is due. Returns control to the scheduler.
- **Exceptions:** one function that sets SRR0/SRR1/MSR and vectors, with each
  cause (DSI, ISI, program, alignment, ...) tested individually.
- **MMU:** `translate(ea, access) -> pa | fault` with BAT → segment → HTAB walk,
  R/C updates, and a soft TLB per access kind. Real-mode fast path.
- **FPU:** correctness first: explicit FPSCR handling, PowerPC NaN rules,
  single-precision rounding. Fast paths only where tests prove equivalence.
- **Host-call hook:** a reserved opcode invokes a registered callback
  (`HostCallHandler`). Used by firmware and by test harnesses.
- **Timebase/decrementer** derived from the scheduler's virtual clock.
- Later: `core/cpu/jit/` (PPC → AArch64) behind the same interface, with the
  interpreter kept as the reference and fallback.

### 3.2 Memory (`core/mem`)
- Guest RAM: one contiguous host allocation, **stored big-endian** (guest byte
  order). Accessors `read32be/write32be` compile to load + `REV`.
- `PhysicalBus`: page-granular (4 KB) map from physical address to either a
  direct RAM/ROM pointer (fast path) or an MMIO handler (`IMmioDevice` with
  8/16/32-bit read/write). Unmapped accesses are logged and return all-ones
  (bus-error behaviour to be verified per region).
- DMA helpers for DBDMA and PCI bus masters operate on physical addresses.

### 3.3 Machine (`core/machine/yosemite`)
Owns and wires all devices, builds the PCI topology (Grackle → slots, DEC21154 →
secondary bus), sets the physical map, and supplies the hardware description
from which firmware builds its device tree. A future second machine type would
be a sibling directory; nothing else changes.

### 3.4 Scheduler (`core/sched`)
- Virtual time in guest bus cycles. Devices post events ("raise IRQ at t+N").
- Main loop: `cpu.run(until next event)` → fire events → repeat.
- Host synchronisation: the Circle app throttles/advances virtual time against
  the real clock for audio and display; tests run unthrottled.

### 3.5 Firmware (`core/firmware/openfirm`)
- **Device tree** built from the machine description and checked against a
  reference dump from a real B&W G3 (tests diff the two).
- **Client interface** (IEEE 1275 PowerPC binding): `finddevice`, `getprop`,
  `getproplen`, `nextprop`, `setprop`, `peer`, `child`, `parent`,
  `instance-to-package`, `package-to-path`, `open`, `close`, `read`, `write`,
  `seek`, `claim`, `release`, `call-method`, `interpret`, `milliseconds`,
  `quiesce`, `exit`. Exact set required by the Trampoline to be confirmed by
  logging every call it makes.
- **Forth engine**: enough of IEEE 1275 Forth to run the `Mac OS ROM` boot script.
- **Boot loader**: reads the HFS/HFS+ boot volume, finds the blessed system
  folder's `tbxi` file, hands it to the Forth engine.
- NVRAM image (OF configuration + Mac OS PRAM) persisted to the SD card.

### 3.6 Platform layer (`platform/`)

| Interface | Circle implementation | Host implementation |
|---|---|---|
| `IDisplay` | `CBcmFrameBuffer`/`CScreenDevice`; conversion of guest pixels → host format on a separate core | memory buffer + PNG dump; optional SDL2 |
| `IInput` | Circle USB keyboard (raw mode) + mouse → host-neutral key/mouse events | scripted events from tests |
| `IAudio` | Circle HDMI sound device, ring buffer | WAV file writer |
| `IBlockDevice` / `IFileSystem` | FatFs on EMMC (disk images are files on the FAT partition) | POSIX files |
| `IHostClock` | Circle timer | `std::chrono` |
| `ILog` | screen + UART + SD log file (+ later network) | stderr |

Guest key codes are produced by translating host USB HID usages to ADB key
codes in `core/devices/macio/adb_keymap` — the core only sees neutral events.

### 3.7 Core assignment on the Pi 400

Now (2026-11-24):

| Core | Work |
|---|---|
| 0 | Circle kernel (interrupts, USB, SD, menu) and the **guest**: CPU, devices, scheduler |
| 1 | Display: the Mac's video memory → HDMI, converted and scaled, changed rows only (`platform/common/screen_presenter`) |
| 2, 3 | idle |

The display hand-off: core 0 publishes a snapshot of the display (mode,
video memory, palette) under a sequence lock and never waits; core 1
renders about 60 times a second. Before the menu draws or the Mac stops,
core 0 pauses core 1 and waits until it is outside its renderer. If core 1
does not start, core 0 draws as before. docs/devlog/2026-11-19-macos921-cd.md.

Planned:

| Core | Work |
|---|---|
| 0 | Circle kernel: interrupts, USB, SD, network, config/debug UI |
| 1 | **Guest**: CPU + devices + scheduler (never blocks) |
| 2 | Display: guest framebuffer → HDMI conversion/scaling |
| 3 | Audio mixing/resampling, disk-image I/O |

Cross-core communication uses lock-free single-producer/single-consumer queues
(input events in, audio samples out, disk requests/results both ways).

### 3.8 Debugging and configuration
- Boot menu (HDMI + keyboard): choose machine config file, disk images, RAM size.
- Config file `g3macpi.ini` on the SD card.
- Built-in monitor: registers, memory dump, disassembly, breakpoints,
  single-step, instruction trace ring buffer, device-state dumps.
  Reachable on screen (hotkey), on UART, and later over the network.

## 4. Testing strategy

| Level | What | Where it runs |
|---|---|---|
| Unit | Each instruction, flags, FPSCR, exceptions, MMU walks, device registers | Linux CI + Claude sessions |
| CPU vectors | Table-driven input/expected-output files in `tests/vectors/` | Linux CI |
| Test ROMs | Small PowerPC programs (built with a PowerPC cross-assembler in CI) that self-check and report via host-call | Linux CI, and on the Pi |
| Firmware | Device tree diff vs real dump; client-interface call scripts | Linux CI |
| Integration | Boot milestones ("reached NanoKernel", "Happy Mac") with screenshot hashes | Linux CI (needs user-supplied Mac OS files → run locally/private only) |
| Performance | Fixed benchmark kernels, MIPS figures logged per commit | Pi + CI |

Apple ROMs and Mac OS files are **never** committed; integration tests that need
them look for them in a path given by an environment variable and skip otherwise.

## 5. Dependencies

- Circle (git submodule, pinned to a release tag) — host infrastructure only.
- Arm GNU toolchain `aarch64-none-elf` (CI and Pi builds).
- Host build: CMake + system GCC/Clang. Test framework: a single-header framework
  vendored in `third_party/` (candidate: doctest, MIT) — decision at M0.
- PowerPC cross-assembler (binutils `powerpc-*`) for building test ROMs and
  later our NDRV — build-time tool only, nothing linked.
