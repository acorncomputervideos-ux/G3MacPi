# Heathrow-family interrupt controller (mac-io) — implementation notes

Code: `core/devices/macio/heathrow_pic.*`. Tests: `tests/unit/devices/test_heathrow_pic.cpp`,
`test_interrupt_path.cpp`.

The Blue & White G3's mac-io chip is Paddington, a Heathrow derivative
(Linux `feature.c`: PowerMac1,1 uses `paddington_features`).

## Register model and its sources

Two independent descriptions were read for facts (no code copied):
Linux `arch/powerpc/platforms/powermac/pic.c` (the driver Mac-compatible
hardware has to satisfy) and QEMU `hw/intc/heathrow_pic.c` (an emulation).
They agree on everything below.

| Item | Value | Linux | QEMU |
|---|---|---|---|
| Register byte order | little-endian 32-bit | `in_le32`/`out_le32` | — |
| Window | 0x40 bytes (Linux ioremap) | yes | 0x1000 window, banks at 0x10/0x20 |
| IRQs 0-31 | bank at offset 0x20 | `addr + 0x20` for bank 0 | inputs 0-31 -> bank at 0x20 |
| IRQs 32-63 | bank at offset 0x10 | `addr + 0x10` for bank 1 | inputs 32-63 -> bank at 0x10 |
| +0x0 event | latched events, read-only | struct field order | yes |
| +0x4 enable | read/write mask | yes | yes |
| +0x8 ack | write 1 to clear latched events | written with the bit | clears events, not level inputs |
| +0xC level | raw input line state | ORed into pending | yes |
| CPU output | (event \| level&level_triggered) & enable | pending = (event\|level)&mask | same as ours |

Our edge inputs latch only on a 0 -> 1 transition (QEMU latches on every
assert call); this is how an edge latch behaves.

## Open points (to verify)
- **Which inputs are level-triggered on the B&W** — wiring-specific. Linux
  learns it from the device tree; QEMU defaults bank 0 bits 20-28 to level.
  We make it configuration (`set_level_triggered`) for the machine model;
  needs a real device-tree dump / Apple documentation.
- The physical address of the controller (inside mac-io's PCI BAR, offset
  0 per QEMU's Old World mac-io) comes with the mac-io/Paddington model.
- Whether the B&W uses a second (cascaded) controller: Linux supports one
  for mac-io; not modelled yet.
