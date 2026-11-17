# Grackle (Motorola MPC106) PCI host bridge — implementation notes

Code: `core/devices/pci/{pci,grackle}.*`. Tests: `tests/unit/devices/test_grackle.cpp`.

## Facts and sources
| Fact | Source |
|---|---|
| B&W G3 host bridge is an MPC106 ("Grackle") | MAME tech-info table, DingusPPC machine file, Linux pci.c comment (G3: one bridge "pci", an MPC106) |
| CONFIG_ADDRESS at 0xFEC00000, CONFIG_DATA at 0xFEE00000 | Linux `setup_grackle()`: `setup_indirect_pci(hose, 0xfec00000, 0xfee00000, 0)` |
| CONFIG_ADDRESS layout = standard PCI mechanism #1 in little-endian (enable bit 31, bus 23-16, devfn 15-8, register 7-2) | Linux `GRACKLE_CFA(b,d,o) = 0x80 \| b<<8 \| d<<16 \| (o&~3)<<24` written with `out_be32`: byte-swapped, exactly that layout |
| CONFIG_DATA little-endian; byte/halfword at data + (offset & 3) | Linux indirect PCI accessors (`in_le32`/`out_le32`, offset & 3) |
| Bridge = bus 0 device 0; PICR1 at config offset 0xA8, LOOPSNOOP bit 0x10 | Linux grackle.c (`GRACKLE_CFA(0, 0, 0xa8)`) |
| PCI IDs 1057:0002 (Motorola MPC106) | PCI ID database |
| Linux reassigns all PCI buses on PowerMac1,1 | `pci_add_flags(PCI_REASSIGN_ALL_BUS)` in setup_grackle |

## Behaviour implemented
- Configuration cycles to bus 0 (type 0); absent functions read all ones
  (master abort), writes ignored; enable bit clear: no cycle.
- Generic `PciConfigSpace`: read-only IDs/class, writable command bits
  (0x57/0x01), cache line size, latency timer, interrupt line; BAR sizing
  per PCI spec (write all ones, read back the size mask).
- Bridge registers 0x80-0xFF (memory-controller and PCI-interface
  configuration) are plain storage.

## Not yet / to verify
- Decode range of the two ports (we map 4 KB each; the MPC106 manual's
  "address map B" ranges must be checked).
- Buses > 0: need the DEC 21154 PCI-PCI bridge model (00:0D.0 per
  DingusPPC's Yosemite definition — to verify with a device-tree dump).
- MPC106 revision ID on the B&W part; memory controller behaviour; error
  status registers (write-1-to-clear); PCI-to-system address maps.
