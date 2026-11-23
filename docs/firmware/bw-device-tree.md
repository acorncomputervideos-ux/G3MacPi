# The Blue & White G3 device tree — sources of every value

Code: `core/firmware/openfirm/bw_device_tree.*`
(`build_bw_device_tree`). Tests: `tests/unit/firmware/test_bw_device_tree.cpp`.

The tree is built from the emulated machine's configuration (RAM size,
clocks, PVR, the mac-io BAR as the firmware assigns it). A real B&W's
device-tree dump would replace the "to verify" values. Until then, every
value has one of these sources:
- **Linux**: what Linux's PowerMac support looks for, read from its
  source (facts only): `arch/powerpc/platforms/powermac/{feature,pci,pic,setup}.c`,
  `drivers/macintosh/{macio_asic,via-cuda}.c`, `drivers/ata/pata_macio.c`,
  `drivers/of/address.c`.
- **750 UM**: the PowerPC 750 user's manual.
- **PCI binding**: the IEEE 1275 PCI bus binding (address cell format).
- **emulator**: the layout our hardware models use, which is itself
  "to verify" in `docs/hardware/paddington.md` and `ata.md`.

| Node / property | Value | Source |
|---|---|---|
| / `name` | `device-tree` | Apple convention as I know it; **to verify** |
| / `model` | `PowerMac1,1` | Linux `pmac_mb_defs` ("Blue&White G3") |
| / `compatible` | `PowerMac1,1`, `MacRISC`, `Power Macintosh` | Linux: `MacRISC`/`Power Macintosh` enable the Mac 1:1 rule for missing `ranges`; full list **to verify** |
| / `#address-cells`, `#size-cells` | 1, 1 | 32-bit PowerPC |
| /cpus/PowerPC,750@0 `device_type` | `cpu` | Linux finds CPUs by it |
| … `clock-frequency`, `bus-frequency` | from the configuration (350/100 MHz) | emulator |
| … `timebase-frequency` | bus / 4 | emulator (timing.md); 750 time base runs at a quarter of the bus clock |
| … `cpu-version` | the emulated PVR | emulator (the real B&W's PVR: **to verify**) |
| … caches | 32 KB I and D, 32-byte blocks, 128 sets (8-way) | 750 UM |
| /memory@0 `reg` | 0, RAM size | emulator |
| /chosen, /aliases, /options, /openprom, /packages | firmware nodes; `/packages/terminal-emulator` carries the console | IEEE 1275; our console placement is a choice |
| /openprom `model` | `OpenFirmware 3` | **to verify** |
| /pci@80000000 | name `pci`, `device_type` `pci`, `bus-range` 0 1 | Linux `pmac_add_bridge` treats a bridge named `pci` as the MPC106 (Grackle) |
| … `compatible` | `grackle` | **to verify** |
| … `reg` 0x80000000 0x7F000000, `ranges` memory 1:1 from 0x80000000, I/O at 0xFE000000 | | **to verify** (MPC106 address map B) |
| /pci/pci-bridge@d | DEC 21154 (1011:0026), `bus-range` 1 1, no `ranges` | emulator; `reg` per the PCI binding |
| …/mac-io@5 | `compatible` `paddington`; `reg` = configuration space, then BAR 0 and its size (PCI binding; the Mac OS ROM loader takes the mac-io base from this second entry and computed 0x10000 without it, so every mac-io device's AAPL,address pointed into RAM); `assigned-addresses` = BAR 0 (32-bit memory, 512 KB); `ranges` child 0 → the BAR | Linux `probe_one_macio` (`assigned-addresses`) and `macio_asic` (`of_address_to_resource` needs `ranges`) |
| … `interrupt-controller`, `#interrupt-cells` 1 | | Linux: NewWorld detection; the PIC is the mac-io node with one-cell specifiers (`irq_domain_xlate_onecell`) |
| …/via-cuda@16000 | `reg` 0x16000 0x2000, `interrupts` 18, `compatible` "cuda" | Linux `find_via_cuda` (by name, `reg`); offset and IRQ: emulator. `compatible`: the Mac OS ROM file's parcel for Cuda machines matches compatible "cuda" + device_type "via-cuda" and adds the PowerMgrPlugin (`pef,AAPL,MacOS,PowerPC,register`, `code,AAPL,MacOS,name`); NativePowerMgrLib imports it. OpenBIOS uses the same compatible |
| …/via-cuda@16000/adb | `device_type` "adb", `compatible` "adb" | Mac OS ROM loader ("HandleSpecialNode: Cuda ADB detected", AAPL,debug). Without it the Mac OS ROM uses its Egret driver (TIP/TACK active high) and hangs waiting for the first shift-register interrupt |
| …/via-cuda@16000/rtc | `device_type` "rtc", `compatible` "rtc" | As OpenBIOS (`drivers/cuda.c`, `rtc_init`). Mac OS 9.2.1's UTCUtils library (System 'nlib' -20878) does a RegistryEntrySearch for device_type "rtc" in its initialisation and fails with nrNotFoundErr (-2539) without it; ControlDefinitions imports UTCUtils, so its load then fails, and patches installed by the library that loaded it point into freed memory (the bomb at the end of start-up). No methods: Mac OS reads the clock through Cuda |
| …/ata@20000, @21000 | `compatible` `heathrow-ata`, `reg` registers (≤ 0x1000) then the DBDMA channel (≤ 0x100), `interrupts` IDE then DMA | Linux `pata_macio` and the macio resource quirks; values: emulator |
| …/ata/disk@0 | `device_type` `block` | IEEE 1275 |
| …/nvram@60000 | `reg` 0x60000 0x20000 | Linux nvram direct mode: stride = size / 8 KB = 16 (as emulated) |
| /aliases | `hd`, `ide0`, `ide1`, `mac-io` | our choice |
| …/escc*/ch-a, ch-b | `slot-names` = 0 on all four channels | OpenBIOS gives this. The Mac OS ROM loader sets extValid1 bits 23/24 when it finds it ("ch-a/ch-b slot-name detected"); without it Mac OS 9.2.1 started LocalTalk on the SCC and waited for ever. To verify against a real B&W tree |
| /pci/usb | `device_type` "usb" only: a placeholder, no USB controller is emulated yet (no `reg`, no driver binds) | Mac OS ROM file loader's special-node table (name "usb", type "usb" → extValid1 bit 26, "has USB"). A real B&W has USB and Mac OS assumes it: without the bit, USBMassStorageLoader calls a NULL 68K hook (ExpandMem+0x294) and Mac OS 9.2.1 stops with System Error 10 |
| /pci/display@10 | our display device: `device_type`/`compatible` "display", PCI-binding `reg` and `assigned-addresses` (video memory, registers), plus `address`, `width`, `height`, `depth`, `linebytes` | our design (docs/hardware/framebuffer.md). Under /pci so that Mac OS maps it from `assigned-addresses`; no configuration space behind it |

Not in the tree yet: audio, floppy; USB only as a placeholder. These hardware models don't exist yet, and a node without a
device would mislead an OS.

## Tests

The tests check the tree the way Linux uses it:
- find the mac-io chip by name and `compatible`;
- take its base from `assigned-addresses`, and translate every mac-io
  cell's `reg` to a CPU address with a translator that follows
  `of_translate_address`, including the Mac 1:1 rule;
- compare the results with the emulator's own constants (Paddington
  offsets, DBDMA channel stride, IRQs, NVRAM window).

So if the tree and the hardware model drift apart, a test fails. A second
build with a different BAR checks that the base flows through.

## "copyright" (root)

"Copyright 1983-1999 Apple Computer, Inc. All Rights Reserved". This is
an interoperability requirement only. Apple's Mac OS ROM loader
(MacOS.elf) stops with "Official Apple copyright message missing." unless
the root's "copyright" starts with "Copyright " and contains " Apple
Computer, Inc.", the two strings it holds. The project owner agreed to
add it, as other emulators' firmware does. Our firmware is not Apple's.

## Additions for Apple's Mac OS ROM loader

Each item was required by the loader, as seen in its client-interface
calls and its own debug output (root `AAPL,debug`):
- **CPU node**: `reservation-granule-size` 32, plus the PowerPC binding's
  TLB properties with the 750's values: `tlb-split`, 256/64 overall,
  128/64 each for I and D. Also `graphics` and `performance-monitor`.
- **Interrupt controller node**: `/pci/pci-bridge/mac-io/interrupt-controller@10`.
  - device_type "interrupt-controller", compatible "paddington", reg
    0x10 0x20, "interrupt-controller", and `#interrupt-cells` 1.
  - Why reg 0x10. The loader takes its "Heathrow base address" from this
    reg. A trace of Mac OS's PIC writes shows it programs IRQs 0-31 at
    base + 0x14 (enable) and + 0x18 (clear), and IRQs 32-63 at base +
    0x04. Linux (pmac_pic.c, from the mac-io base) has the IRQ 0-31 bank
    at mac-io + 0x20 and IRQs 32-63 at + 0x10. Both agree only with the
    base at mac-io + 0x10. With reg 0 Mac OS enabled the VIA's interrupt
    (18) as IRQ 50 and never received it. A real B&W dump would confirm.
  - The loader's table of special nodes recognises an interrupt
    controller by that device_type plus compatible "heathrow" or
    "paddington".
  - mac-io's children name it in `interrupt-parent` (IEEE 1275 interrupt
    binding), and `/chosen` `interrupt-controller` holds its phandle.
  - Linux skips `/chosen/interrupt-controller`; the loader passes the
    value to getprop, hence a phandle.
- **`/rtas`**: see rtas.md.
- **Path lookup**: a component may be `@unit` with no name, matching any
  child at that address (IEEE 1275). The loader looks up `/cpus/@0`.

## What Mac OS's ATA driver needs

Found by disassembling the Mac OS ROM's `heathrow-ata` driver (a PEF
container at ROM 0xFFDF6970; its data section decompressed with the PEF
pattern-initialised-data rules) and watching it run (`G3_CODEWATCH`):

- **mac-io `device_type "mac-io"`.** The driver searches for a node with
  device_type "dbdma" or "mac-io" and reads its `AAPL,address` (Mac OS
  derives that from `assigned-addresses`). Without it the driver returns
  error -56 before touching the controller.
- **ata nodes:** `AAPL,address` (8 bytes, derived by Mac OS from `reg`),
  `AAPL,interrupt-vectors` or `AAPL,interrupts` (8 bytes, derived from
  `interrupts`), **`AAPL,pio-timing` (at least 5 words)**, `driver-ist`
  (24 bytes, made by the loader). `AAPL,pio-timing` and `AAPL,bus-id`
  carry the values OpenBIOS gives these nodes (to verify on a real B&W).
- With these, Mac OS sends IDENTIFY DEVICE, INITIALIZE DEVICE PARAMETERS
  and READ SECTORS (block 0, the driver descriptor map) to a disk on
  channel 0.

A real B&W has one mac-io ATA channel (CD-ROM, Zip; Linux: "Heathrow ATA
controller (macio), bus ID 0", irq 13) and its hard disk on a CMD646 PCI
controller (`/pci@80000000/pci-bridge@d/pci-ata@1/ata-4@0`). We model
two mac-io channels and no CMD646 yet; Mac OS drives a disk on the mac-io
channel through the ROM's heathrow-ata driver.
