# The display in the firmware — implementation notes

Code: `core/firmware/openfirm/display_driver.*`, the display node in
`bw_device_tree.cpp`, and the wiring in `core/machine/bw_firmware.cpp`.
Tests: `tests/unit/machine/test_bw_display.cpp`. The device itself:
`docs/hardware/framebuffer.md`.

## In the machine

`YosemiteConfig::vram` and `vram_size` (at most 16 MB) give the machine
our display device:
- video memory at 0x90000000;
- registers at 0x91000000.

Both are in PCI memory space, outside every PCI BAR. At power-on the
firmware sets the configured mode (default 640×480, depth 8), clears the
screen to black and enables it.

## Device tree node `/display@90000000`

The properties are what Linux's `offb` driver reads
(`drivers/video/fbdev/offb.c`):

| Property | Value |
|---|---|
| `compatible` | `display` (offb's match table) |
| `device_type` | `display` |
| `width`, `height`, `depth`, `linebytes` | the active mode |
| `address` | the frame buffer's address |
| `reg` | video memory: address and size (root level, so 1:1) |

The alias `screen` points to it.

Mac OS will also need a display driver ("NDRV") property on this node.
That is step 3d.

## Methods

IEEE 1275 display device methods, from my knowledge of the standard
(to verify). The `color!` argument order is confirmed by Linux's
`prom_init`: `call-method("color!", ih, i, b, g, r)` means
( r g b index -- ) on the Forth stack.

| Method | Stack effect |
|---|---|
| `color!` | ( r g b index -- ) |
| `color@` | ( index -- r g b ) |
| `set-colors` | ( adr index #indices -- ): adr holds r,g,b byte triples |
| `fill-rectangle` | ( index x y w h -- ), clipped to the screen |
| `dimensions` | ( -- width height ) |

In 15- and 32-bit modes a colour index stands for its palette entry.
Drivers can now provide any method through
`PackageDriver::method()`; `$call-method` and the client interface's
`call-method` reach it.

## Tests

- The node as offb reads it; the power-on picture is enabled and black,
  with nothing written past it.
- Each method from Forth, including clipping at the screen edge, and the
  converted picture.
- Linux's exact `color!` call through the client interface.
- 32- and 15-bit modes (pixel bytes checked). A mid-range colour was
  added after a sabotage run showed that pure red can't detect a wrong
  shift.
- A machine without video memory has no display node and no alias.
