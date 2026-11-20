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

## Interrupt mode (bit 31)

Source: DingusPPC's MacIoTwo model (`devices/ioctrl/maciotwo.cpp`,
`maciobase.cpp`; facts only), an emulator that boots Mac OS on these
machines. Neither Linux nor QEMU uses the bit. To verify on hardware.

- Bit 31 of the IRQ 0-31 enable register (+0x24) is IntMode: 0 = native,
  1 = "68K-style". It enables no source.
- It reads back as bit 31 of the IRQ 32-63 enable register (+0x14), which
  ignores writes to that bit.
- In 68K mode an edge input latches an event on both transitions, and
  writing bit 31 to the IRQ 0-31 ack register (+0x28) clears every event.
- Mac OS 9 sets the mode and acknowledges with bit 31. Before this was
  modelled the VIA's event stayed latched and the CPU took millions of
  external interrupts (devlog 2026-11-18).

## Open points (to verify)
- **Which inputs are level-triggered on the B&W** — wiring-specific. Linux
  learns it from the device tree; QEMU defaults bank 0 bits 20-28 to level.
  We make it configuration (`set_level_triggered`) for the machine model;
  needs a real device-tree dump / Apple documentation.
- The physical address of the controller (inside mac-io's PCI BAR, offset
  0 per QEMU's Old World mac-io) comes with the mac-io/Paddington model.
- Whether the B&W uses a second (cascaded) controller: Linux supports one
  for mac-io; not modelled yet.

## DMA interrupt sources (IRQs 0-10)

- A DBDMA channel only raises its interrupt line. Software lowers it by
  writing the source's bit to the IRQ 0-31 ack register. In 68K mode that
  1 -> 0 change also latches an event. The ack-all (bit 31) does not lower
  DMA lines.
- Source: DingusPPC's MacIoTwo/GrandCentral `clear_dma_int`, read for facts
  only; QEMU instead never lowers its DBDMA line. To verify on hardware.
- Why it matters: Mac OS's nanokernel acks every event with bit 31 and then
  dispatches from the level register. A DMA interrupt that dropped by
  itself was never seen. Mac OS 9.2.1's heathrow-ata driver therefore never
  counted a DMA read's bytes, its CD driver retried every read, and it
  ejected the boot CD. Holding the line until the ack fixed the boot; with
  a line that stayed up for ever the same ack loop became an interrupt storm.
