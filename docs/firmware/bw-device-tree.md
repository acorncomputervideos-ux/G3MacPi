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
| …/mac-io@5 | `compatible` `paddington`; `assigned-addresses` = BAR 0 (32-bit memory, 512 KB); `ranges` child 0 → the BAR | Linux `probe_one_macio` (`assigned-addresses`) and `macio_asic` (`of_address_to_resource` needs `ranges`) |
| … `interrupt-controller`, `#interrupt-cells` 1 | | Linux: NewWorld detection; the PIC is the mac-io node with one-cell specifiers (`irq_domain_xlate_onecell`) |
| …/via-cuda@16000 | `reg` 0x16000 0x2000, `interrupts` 18 | Linux `find_via_cuda` (by name, `reg`); offset and IRQ: emulator |
| …/ata@20000, @21000 | `compatible` `heathrow-ata`, `reg` registers (≤ 0x1000) then the DBDMA channel (≤ 0x100), `interrupts` IDE then DMA | Linux `pata_macio` and the macio resource quirks; values: emulator |
| …/ata/disk@0 | `device_type` `block` | IEEE 1275 |
| …/nvram@60000 | `reg` 0x60000 0x20000 | Linux nvram direct mode: stride = size / 8 KB = 16 (as emulated) |
| /aliases | `hd`, `ide0`, `ide1`, `mac-io` | our choice |

Not in the tree yet: the SCC serial ports, audio, floppy, USB, the
display. These hardware models don't exist yet, and a node without a
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
