# The display device (framebuffer) — implementation notes

Code: `core/devices/video/framebuffer.*`. Tests: `tests/unit/devices/test_framebuffer.cpp`.

The real Blue & White G3 has an ATI Rage 128 graphics card. Emulating it
is planned for later (M9), because Mac OS's own ATI drivers need it. For
the first Mac OS boots we use our own simple display device:
- **Video memory** is plain RAM on the bus, so the CPU draws at full speed.
- **A small register block** sets the mode and the palette.

Our firmware and our own Mac OS display driver are the only software that
programs it, so its interface is our design. It imitates no real card.

It isn't a PCI device: a PCI function would need a vendor ID, and we must
not borrow another company's. Its device-tree node sits under `/pci`
(Grackle) as device 0x10, with PCI-binding `reg` and `assigned-addresses`
(entry 0 = video memory at 0x90000000, a power of two in size; entry 1 =
the register block at 0x91000000). There is no configuration space
behind it.

Why under `/pci`: Mac OS maps a device's memory and gives its driver
logical addresses (`AAPL,address`) from the node's `assigned-addresses`.
As a root node with physical `reg`, Mac OS 9.2.1 left it unmapped, and
our driver's first register read faulted (DSI at 0x91000000). That ended
in System Error 11. Under `/pci` it is mapped and drawn on, without any
configuration space (tried both ways).

## Pixel formats

The depth values are those Linux's `offb` driver uses for Open Firmware
displays (`drivers/video/fbdev/offb.c`). Pixels are big-endian, like all
PowerPC frame buffers.

| depth | bits/pixel | layout |
|---|---|---|
| 8 | 8 | index into a 256-entry palette (0x00RRGGBB entries) |
| 15 | 16 | x1 R5 G5 B5 |
| 32 | 32 | x8 R8 G8 B8 |

Pitch = width × bytes per pixel, rounded up to 32 bytes.

## Registers

32-bit, big-endian; narrower reads return the matching bytes, narrower
writes are ignored.

| Offset | Name | Access | Meaning |
|---|---|---|---|
| 0x00 | ID | RO | 0x47334642 ('G3FB') |
| 0x04 | VERSION | RO | 2 (1: before the interrupt registers) |
| 0x08 / 0x0C / 0x10 | WIDTH / HEIGHT / DEPTH | RW | staged mode |
| 0x14 | PITCH | RO | bytes per row of the active mode |
| 0x18 | ENABLE | RW | bit 0: show the picture |
| 0x1C | VRAMSIZE | RO | bytes of video memory |
| 0x20 | PALINDEX | RW | palette index |
| 0x24 | PALDATA | RW | palette entry 0x00RRGGBB; the index increments after each access |
| 0x28 | STATUS | RO | 1 = the last APPLY was accepted |
| 0x2C | APPLY | WO | staged mode becomes active, if valid |
| 0x30 | FRAMES | RO | frames shown so far |
| 0x34 | IRQ_ENABLE | RW | bit 0: vertical-blank interrupt on |
| 0x38 | IRQ_STATUS | RO / write 1 to clear | bit 0: a vertical blank happened |

A mode is valid if width ≤ 1920, height ≤ 1200, the depth is supported,
and pitch × height fits in video memory. A rejected mode leaves the
active one unchanged. The power-on palette is a grey ramp (0 black, 255
white).

## Vertical-blank interrupt

While IRQ_ENABLE bit 0 is set, IRQ_STATUS bit 0 is set every 1/60 s of
guest time (60 Hz is our choice; the next blank is planned from the last
one, so the rate holds however late an event runs). The interrupt line is
high while IRQ_STATUS & IRQ_ENABLE bit 0 is; writing 1 to IRQ_STATUS bit 0
clears it. Clearing IRQ_ENABLE drops the line and stops the timer; the
next enable gives a blank a full period later.

The line is Paddington input 22 (our choice: no emulated device uses it;
not a claim about a real B&W slot). The `display` node carries
`interrupts = 22` and `interrupt-parent` = the interrupt controller, like
the CMD646's, so Mac OS makes a `driver-ist` property for it and our
driver installs its handler there (docs/guest/ndrv.md).

## Host side

`convert()` turns the active picture into one 0x00RRGGBB word per pixel.
The Pi side will blit that to HDMI. 5-bit channels are expanded to 8
bits by repeating their top bits, so full intensity gives 0xFF.

## Tests

- The vertical-blank interrupt: none while off; the line rises at each
  1/60 s and stays up until acknowledged (writing 0 clears nothing);
  turning it off drops the line and stops the timer; on again, the next
  blank is a full period later.
- Byte order of the registers; read-only registers; valid modes and
  pitches; every rejection rule; the palette's auto-increment and wrap.
- Pixels written by CPU-style bus accesses, converted in all three depths
  (8-bit through the palette, 15-bit channel expansion, 32-bit); a wider
  destination; overlapping mappings refused.

## Solved: 256 colours stopping at "Starting Up"

With version 1 (no interrupt) our driver stood in for vertical blanking
with a Driver Services timer re-armed from its own handler every 16 ms.
Mac OS 9.2.1 started from a hard disk in 256 colours sometimes stopped at
"Starting Up" for good (2 of 18 g3_host runs at different CPU speeds; on
the Pi it depends on timing). What a trace showed (`G3_TIME` makes runs
repeat exactly):

- The Mac OS timer task (the one that runs interrupt timers) sleeps with
  NanoKernel call 55 (MPDelayUntilSys, "delay until", per the
  powermac-rom NanoKernel sources' labels) until the earliest timer.
- At the moment of the stop it slept twice at the same instant, and from
  then on it only ever waited for our VBL timer: every other timer was
  gone. The Ticks counter (low memory 0x16A) stopped, the Ethernet
  driver's polling stopped, and the main Mac OS task went idle waiting
  for them.
- The JIT is not involved (the same run stops with it off).

Why the other timers were lost is not known. Real Mac video cards signal
vertical blanking with a hardware interrupt, not a timer, so version 2
gives the device one (above) and the driver signals the 'vbl ' service
from its interrupt handler; it no longer uses interrupt timers. Since
then: 13 of 13 runs at the CPU speeds above reach the desktop in 256
colours, as do thousands and millions; the pointer moves and menus open.

