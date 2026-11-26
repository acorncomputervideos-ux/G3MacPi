# 01 — Research and Feasibility Study

Status: **Phase 0 (research) — no emulator code written yet.**
Last updated: 2026-11-16

This document records what was researched, what was confirmed from sources,
what is still unverified, and the obstacles that shape the architecture.
Every hardware fact is tagged:

- **[confirmed]** — checked against a cited source during this research pass.
- **[known]** — well-established architectural fact (PowerPC/ARM manuals).
- **[verify]** — believed true, but must be confirmed against a primary source
  (Apple developer note, a real device-tree dump, or a datasheet) *before*
  any code depends on it.

---

## 1. Verdict

**The project is technically feasible, with three caveats that must be accepted up front:**

1. **Performance.** A pure interpreter on a Cortex-A72 will run Mac OS 9 well
   below the speed of a real 350 MHz G3. Booting and using the Finder is
   realistic; "fast" is not, until a dynamic recompiler exists (Milestone 8).
2. **Firmware.** Mac OS 9 on a Blue & White G3 is a *New World* boot. It needs
   an Open Firmware environment that can run Apple's `Mac OS ROM` boot script
   and serve the Trampoline's client-interface calls. We must write this
   ourselves. This is the single largest unknown in the project.
3. **Some target hardware will be staged, not emulated on day one.** The ATI
   Rage 128 and the USB (OHCI) controller are large, poorly documented devices.
   The plan reaches a bootable Mac OS 9 first via simpler, still-genuine B&W G3
   paths (Paddington ATA, ADB via Cuda, a framebuffer with our own display
   driver) and adds Rage 128 / USB / CMD646 afterwards.

None of these is a showstopper. All are explained below.

---

## 2. Host: Raspberry Pi 400 bare-metal

| Item | Finding | Tag |
|---|---|---|
| SoC | BCM2711, 4× Cortex-A72 (ARMv8-A), 1.8 GHz on Pi 400 | [known] |
| RAM | 4 GB LPDDR4 | [known] |
| Boot | VideoCore firmware (`start4.elf`, `fixup4.dat`) loads our kernel image from the FAT boot partition; `config.txt` selects 64-bit mode (`arm_64bit=1`) | [known] |
| Interrupt controller | GIC-400 (Circle handles it) | [known] |
| USB | VL805 xHCI controller on PCIe. The Pi 400's own keyboard is a USB device on this bus. | [known] |
| SD card | EMMC2 controller | [known] |
| Display | 2× micro-HDMI | [known] |
| **Audio** | **The Pi 400 has no 3.5 mm analogue jack.** Audio must go out over **HDMI** (or a USB audio device). | [known] |
| Serial | GPIO14/15 UART on the 40-pin rear header; needs a 3.3 V USB-serial adapter | [known] |
| Ethernet | Gigabit (BCM54213PE / GENET) | [known] — Circle driver support on Pi 4/400 **[verify]** in M1 |

### Circle framework (checked: `rsta2/circle` README, Step 51.x)

- Raspberry Pi 4B and **Pi 400 are listed as tested** [confirmed].
- **64-bit (AArch64) builds supported** on Pi 3/4/5 (`AARCH=64`, `RASPPI=4`) [confirmed].
- Memory map documents a **high heap above 1 GB** (up to 3 GB) on Pi 4 AArch64 [confirmed].
  Our 128 MB–512 MB guest RAM fits comfortably in the normal heap anyway.
- **HDMI sound output (without VCHIQ)** supported [confirmed] — this is our audio path.
- USB host stack with HID keyboard/mouse drivers, plug-and-play [confirmed].
- EMMC SD driver + **FatFs** (full-function FAT) [confirmed].
- Multi-core support (`CMultiCoreSupport`) on Pi 2/3/4 [confirmed].
- UART driver, TCP/IP stack, HTTP server, syslog-over-UDP client [confirmed].
- Sample **38-bootloader: HTTP/TFTP bootloader with a web front-end** [confirmed]
  — potentially lets an iPad upload a new kernel from Safari with no SD-card swap.
- **Licence: Circle is GPL-3.0.** Because the final kernel image links Circle,
  this project must be distributed under a GPL-3.0-compatible licence. [verify licence text when vendoring]

**Conclusion:** Circle covers every host-side need (display, USB input, SD/FAT,
HDMI audio, serial, multicore, network). Circle is used strictly as host
infrastructure; no guest emulation comes from it.

---

## 3. Guest CPU: PowerPC 750 (G3)

[known] from the *MPC750 RISC Microprocessor User's Manual* and *PowerPC
Programming Environments Manual for 32-bit Implementations* (PEM):

- 32-bit PowerPC (UISA/VEA/OEA), **big-endian** in Mac use. **No AltiVec** (that is G4).
  No 64-bit instructions. Paired-single (Gekko) is **not** part of the 750.
- Registers: GPR0–31, FPR0–31 (64-bit), CR, XER, LR, CTR, FPSCR, MSR,
  16 segment registers, SDR1, IBAT0–3/DBAT0–3 (upper/lower), SRR0/1, SPRG0–3,
  DAR, DSISR, DEC, TBL/TBU, PVR, HID0/HID1, L2CR, ICTC, THRM1–3,
  MMCR0/1, PMC1–4, SIA, IABR, DABR, EAR.
- MMU: **segmented, hashed page table** (HTAB via SDR1, primary/secondary hash,
  8-entry PTEGs, R/C bit updates) **plus BAT** translation; real mode when MSR[IR/DR]=0.
- Exceptions: reset 0x100, machine check 0x200, DSI 0x300, ISI 0x400,
  external 0x500, alignment 0x600, program 0x700, FP unavailable 0x800,
  decrementer 0x900, system call 0xC00, trace 0xD00, performance monitor 0xF00,
  instruction-address breakpoint 0x1300, system management 0x1400,
  thermal 0x1700. Vector base 0xFFF00000 when MSR[IP]=1.
- Time base and decrementer tick at **bus clock / 4** on 750 systems.
  For a 100 MHz bus that is 25 MHz. The OS learns the rate from the
  device tree (`timebase-frequency`), so emulated values just need to be
  self-consistent. [known; exact B&W device-tree values: verify]

### CPU emulation obstacles

| Problem | Impact | Mitigation |
|---|---|---|
| Endianness (BE guest, LE host) | Every load/store | Keep guest RAM in guest (big-endian) byte order; byte-swap on access with `REV` (1 cycle on A72). Simple, DMA-safe, framebuffer-safe. |
| FP semantics | NaN propagation, FPSCR sticky/exception bits, `fmadds` double-rounding, `fres`/`frsqrte` estimate tables, denormals | Accurate path first (softer but correct), with an AArch64 hardware-FP fast path where provably identical. AArch64 has true fused multiply-add, which matches PowerPC `fmadd`. Mac OS 9 rarely enables FP exceptions, but FPSCR flags must be right. |
| MMU cost | Every access translates | Software TLB (direct-mapped, per access type) in front of the BAT/HTAB walk; flush on `tlbie`, `mtsr`, `mtsdr1`, BAT writes, MSR changes. |
| Interpreter speed | Primary performance limit | See §7. |
| No reference G3 hardware | Test oracle needed | Hand-verified vectors from the manuals + optional golden vectors from real hardware or an *offline* reference (decision needed — see §9). |

---

## 4. Guest machine: Power Macintosh G3 (Blue & White)

Model identifier **`PowerMac1,1`**, board code-name **Yosemite** [confirmed:
Linux `arch/powerpc/platforms/powermac/feature.c` maps `"PowerMac1,1"` →
`"Blue&White G3"`, `PMAC_TYPE_YOSEMITE`, `paddington_features`].

### Component inventory

| Function | Part | Source / tag |
|---|---|---|
| Host bridge + memory controller | Motorola **MPC106 "Grackle"** | MAME tech-info table, DingusPPC machine file [confirmed ×2] |
| PCI-PCI bridge | **DEC 21154** at 00:0D.0 | DingusPPC [confirmed], MAME [confirmed] |
| I/O controller ("mac-io") | **Paddington** (Heathrow-family) | Linux feature table, MAME [confirmed] (DingusPPC instantiates a Heathrow-derived class) |
| Interrupt controller | Heathrow-style mac-io PIC (not OpenPIC) | [verify — strongly implied by Paddington; Linux `pmac_pic.c`] |
| DMA | Apple DBDMA channels inside Paddington | [verify channel map] |
| ADB / RTC / power / PRAM | **Cuda** (6805 µC via VIA) | MAME table [confirmed]; B&W has one ADB port [known] |
| NVRAM | 8 KB, Open Firmware + Mac OS PRAM partitions | [verify access method] |
| Serial | Zilog SCC (85C30-family) in Paddington | [known] |
| Floppy | SWIM3 cell (no drive fitted on B&W) | Linux feature table [confirmed] |
| ATA (CD-ROM) | Paddington internal ATA | MAME [confirmed] |
| Ultra ATA (hard disk) | **CMD646** (rev 1) / **CMD646U2** (rev 2) PCI | MAME, DingusPPC [confirmed] |
| Sound codec | **Burgundy** | MAME, DingusPPC [confirmed] |
| USB | 2-port OHCI PCI controller (behind DEC21154, device 6) | DingusPPC slot list [confirmed]; chip model [verify] |
| FireWire | PCI 1394 controller (behind DEC21154, device 0) | DingusPPC [confirmed]; out of scope |
| Graphics | **ATI Rage 128 GL** card in 66 MHz slot J12 | [known]; DingusPPC defaults a Mach64 GX in that slot |
| Boot ROM | **1 MB at 0xFFF00000** | DingusPPC [confirmed] |
| RAM | 4 DIMM slots (SPD over I²C) | DingusPPC [confirmed] |
| CPU/bus | 750 at 300/350/400/450 MHz, 100 MHz bus | [known]; DingusPPC uses a 66.82 MHz bus — we will **not** copy that; device tree values will be our own, consistent choice |

Grackle CHRP-style map: PCI config address port **0xFEC00000**, data port
**0xFEE00000** (Linux `setup_grackle()` uses these) [verify against MPC106 manual].
PCI memory BARs (including mac-io) are *assigned by firmware*, so the
addresses are ours to choose as long as the device tree reports them.

**Important discrepancy found:** reference sources disagree on details
(e.g. Heathrow vs Paddington naming, bus speed). This confirms the brief's
rule: we need a **primary-source device tree dump** from a real B&W G3
(Open Firmware `dev / ls` + `.properties`, or `ioreg`/`/proc/device-tree`
under Mac OS X/Linux). Obtaining one is task #1 of Milestone 4.

---

## 5. Firmware and the New World boot process

From Apple Technote **TN1167 "NewWorld Architecture"** [confirmed summary]:

1. The 1 MB boot ROM contains only **Open Firmware** (plus POST/hardware init).
   The Mac OS "toolbox ROM" is **no longer in hardware**.
2. OF finds the boot volume's `\\:tbxi` file — the **`Mac OS ROM`** file in the
   System Folder. That file (a CHRP "bootinfo") contains:
   - a **Forth boot script**,
   - the **Trampoline** (PowerPC code),
   - the **compressed Mac OS ROM image** (NanoKernel, 68k emulator, toolbox, HAL drivers).
3. OF runs the script, which loads the Trampoline and ROM image into memory.
4. The **Trampoline** decompresses the ROM image, walks the **Open Firmware
   device tree through the client interface** to build the Mac OS
   *Name Registry* and NanoKernel structures, quiesces OF, rearranges memory,
   and jumps to the ROM's `HardwareInit`, which starts the **NanoKernel**.

### Consequences

- **We do not need a dump of the hardware boot ROM to boot Mac OS 9.** The
  user only needs a legally owned Mac OS 9 install (which contains `Mac OS ROM`).
  This is the strongest argument for writing our own firmware.
- A *generic* PowerPC Open Firmware will **not** boot Mac OS 9 (brief §5 is right).
  The device tree must look like a real B&W G3: correct node names,
  `compatible`, `model = "PowerMac1,1"`, `reg`/`assigned-addresses`,
  `interrupts`, `AAPL,*` properties, clock frequencies, memory/`available`, etc.
  The Mac OS ROM image's HAL then drives the emulated chips **directly** —
  so the emulated hardware must behave like the real chips.
- The boot script is Forth and checks the machine's `compatible`/model list.
  We need a **Forth interpreter sufficient for that script**, not a full IEEE 1275
  implementation.
- Evidence that this works in principle: QEMU's `mac99` machine boots Mac OS 9
  with the non-Apple OpenBIOS firmware (reported on LKML, Nov 2025). Note it is
  a *G4/UniNorth* machine, not a B&W. **We may read such projects for
  hardware facts but will not copy code** (and QEMU/Linux GPL-2-only code is
  licence-incompatible with GPL-3 anyway).

### Firmware design choice (recommended)

**HLE Open Firmware written in C++, running on the host**, connected to the guest by a
trap: the client-interface entry point in guest memory contains a reserved
"host call" opcode that the CPU hands to a registered callback. The CPU core
knows nothing about Open Firmware (meets brief §5 "keep firmware separate").
A small Forth engine executes the bootinfo script. A later, optional path is
running a user-dumped real B&W boot ROM (LLE), which is a much stricter test of
hardware accuracy.

---

## 6. Mac OS 9 compatibility problems (anticipated)

| # | Problem | Why | Plan |
|---|---|---|---|
| 1 | Device-tree fidelity | Trampoline + HAL consume it | Build from a real dump; diff against it in tests |
| 2 | **Display driver** | Mac OS 9 needs an `NDRV` display driver. On real hardware it lives in the Rage 128 card's option ROM (ATI, proprietary). | Stage 1: simple emulated framebuffer PCI device **plus our own NDRV** (PowerPC PEF code we write). Stage 2: Rage 128 register emulation for ATI's driver. |
| 3 | Hard disk driver | DingusPPC reports that B&W HD boot fails without CMD646 emulation; Paddington ATA is the better-understood path | Put the boot disk on Paddington ATA first; CMD646 later |
| 4 | Keyboard/mouse | USB needs a full OHCI + guest HID stack | **ADB via Cuda first** (the B&W has an ADB port and Mac OS 9 supports it). Guest USB OHCI later. Host side is still the Pi's USB keyboard/mouse. |
| 5 | Interrupt routing | Wrong PIC behaviour = hang at boot | Implement Heathrow-family PIC from Linux `pmac_pic.c` semantics + tests |
| 6 | Cuda protocol | RTC, PRAM, power, ADB autopoll all go through it | Implement from Cuda protocol docs (Linux `via-cuda.c` as reference for behaviour) |
| 7 | Timing | DEC/TB rates vs. emulated speed; OS 9 calibrates loops | Deterministic virtual clock; device-tree frequencies consistent with it |
| 8 | Mac OS version | B&W G3 shipped with 8.5.1; OS 9.0–9.2.2 support it | Target **Mac OS 9.2.2** first, 9.1/9.0.4 second [verify each version's `Mac OS ROM` lists PowerMac1,1] |
| 9 | Disk images | Mac images often lack a partition map (raw HFS) | Support Apple Partition Map images first; wrap raw HFS volumes later |
| 10 | Sound | Burgundy is poorly documented | Last of the core devices; boot without it (Mac OS tolerates silent hardware, [verify]) |
| 11 | Cache behaviour | Mac OS 9 uses `dcbz`, `dcbf`, `icbi`, L2CR setup | Functional emulation (no cache model); `dcbz` must zero 32 bytes; `icbi` must invalidate JIT/decoded-instruction caches later |

---

## 7. Performance estimate (to be replaced by measurements)

These are planning numbers, not promises:

- A well-built switch/table interpreter typically costs ~15–40 host cycles per
  guest instruction including MMU lookup. At 1.8 GHz that is roughly
  **45–120 million guest instructions/second**.
- A real 750 at 350 MHz sustains very roughly 1 instruction/cycle on
  integer code, i.e. a few hundred MIPS.
- So an interpreter is expected to behave like a G3 at **very roughly 15–35 %**
  of real speed. Mac OS 9 boot is still viable at that speed; Finder will be
  sluggish but usable; heavy applications will be slow.
- A dynamic recompiler (PPC → AArch64) is the standard way to gain 3–10× and
  is feasible on bare metal (we own the MMU, so writable+executable code
  buffers and `IC IVAU`/`DC CVAU` maintenance are straightforward).
- The guest CPU is single-core, so one A72 core runs the CPU; the other three
  handle host I/O, display conversion and audio so they never steal CPU-core time.

Milestone 3 measures real numbers with a benchmark harness that runs both on
the Pi and on the host CI.

---

## 8. Development-workflow obstacle (important)

The brief says the Pi 400 compiles **and** runs the emulator. A bare-metal
emulator *replaces* the OS, so the Pi cannot be compiling under Linux while it
is running the emulator. Options:

| Option | How | Verdict |
|---|---|---|
| **A. Cloud build (recommended)** | GitHub Actions cross-compiles with the Arm GNU toolchain (`aarch64-none-elf`), runs all host unit tests, publishes `kernel8-rpi4.img` as an artifact. The iPad downloads it. | Works entirely from an iPad; every commit is tested |
| B. Network deploy | Pi boots Circle's HTTP bootloader; iPad Safari uploads the kernel image | Removes SD swapping; evaluate in M1 |
| C. Two SD cards | One Raspberry Pi OS card for compiling on the Pi, one emulator card | Works, but slow and manual |

Debug output must also reach an iPad: on-screen HDMI console, a log file on
the SD card, and later an HTTP/telnet debug console over Ethernet. Serial
remains available for people with a USB-serial adapter.

**Key architectural consequence:** the emulator core must be **portable C++
with no Circle dependency**, so it builds and is tested on Linux in CI (and in
Claude sessions). Only a thin platform layer touches Circle.

---

## 9. Decisions needed from the project owner

1. **AArch64 vs 32-bit Circle build.** Recommend **AArch64** (more registers,
   better JIT target, high memory).
2. **Firmware path.** Recommend **own HLE Open Firmware** first; real-ROM LLE later and optional.
3. **Staging of input/graphics/disk** (ADB, simple framebuffer + own NDRV,
   Paddington ATA first). Recommended.
4. **Test oracle.** May golden CPU test vectors be generated *offline* from
   real PowerPC hardware or an existing emulator used purely as a reference
   (never linked or copied)? Recommended yes; otherwise tests come only from
   manual-derived vectors.
5. **Licence.** Must be GPL-3.0-compatible because of Circle.
   Recommend **GPL-3.0-or-later**.
6. **Deployment.** Cloud build + SD swap first, HTTP bootloader evaluated in M1.

---

## 10. Sources consulted in this pass

- Circle README and samples list — https://github.com/rsta2/circle
- Circle memory map — `doc/memorymap.txt` in the Circle repository
- Linux PowerMac feature table — `arch/powerpc/platforms/powermac/feature.c`
- DingusPPC Yosemite machine definition (read for facts only) — https://git.applefritter.com/Macintosh-Tools/dingusppc/raw/branch/master/machines/machineyosemite.cpp
- MAME Macintosh tech-info table — https://wiki.mamedev.org/index.php/Driver:Mac_68K:Tech_Info
- Apple TN1167 "NewWorld Architecture" (via ADC Reference Library mirrors) and https://en.wikipedia.org/wiki/New_World_ROM
- LKML thread on QEMU mac99 + OpenBIOS booting Mac OS 9 — https://lkml.iu.edu/2511.0/08383.html
- OpenBIOS mailing-list threads on Mac device trees — https://mail.coreboot.org/pipermail/openbios/2013-January/007372.html

Still to obtain (see `docs/references.md`): MPC750 User's Manual, PowerPC PEM,
MPC106 User's Manual, Apple "Power Macintosh G3 (Blue and White)" Developer
Note, IEEE 1275 + PowerPC binding + CHRP binding, a real B&W G3 device-tree dump,
Cuda and DBDMA documentation, *Designing PCI Cards and Drivers for Power Macintosh*.
