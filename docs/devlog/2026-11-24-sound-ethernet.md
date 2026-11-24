# 2026-11-24 — Sound and Ethernet cells, served by core 3

## What was added

- **DAV bus sound cell** (`core/devices/macio/davbus`): Screamer and
  Burgundy codec protocols, output/input DMA paced by the codec clock, the
  frame count register as a clock. docs/hardware/davbus.md.
- **BMAC+ Ethernet cell** (`core/devices/macio/bmac`): registers, MII PHY,
  transmit and receive rings. docs/hardware/bmac.md.
- **DBDMA**: packet devices can end an INPUT_LAST descriptor early, with the
  residual count (`DbdmaStream::dma_input_packet_end`).
- **PIC**: IRQs 32/33 (Ethernet DMA) latch like the other DMA lines.
- **Paddington / device tree**: `davbus`/`sound` and `ethernet` nodes;
  `sound`/`network` settings (INI and menu).
- **Bridges** (`platform/common/audio_bridge`, `net_bridge`): lock-free
  queues between the emulation core and core 3; audio resampled to the
  output rate with bounded latency.
- **Pi**: core 3 also keeps the HDMI sound queue filled and moves Ethernet
  frames to and from the Pi's adapter (Circle's BCM54213 driver, polled). Its
  WFE is woken every ~150 us by the generic timer's event stream, as sound
  and network have nobody to send it an event. The Mac gets the Pi's own
  Ethernet address.
- **g3_host** (experiments): `G3_WAV=file` writes the Mac's sound;
  `G3_NETLOG=1` lists frames sent; `G3_KEYS=sec:usage+usage,...` presses
  keys; `G3_MMIO_RANGE=lo,hi[,max]` traces register accesses (repeated
  reads collapsed; DBDMA command lists dumped); `G3_XPRAM=off:val` pokes the
  Mac OS PRAM partition.

## Findings with Mac OS 9.2.1 (from the CD)

- The sound driver measures the codec: its output DMA lists read the frame
  count register with LOAD_WORD before and after each 2048-byte block. With
  a frame count that only moved when data was consumed, and a FIFO that
  took a block at once, the codec looked dead. Now the count is a clock and
  the FIFO 64 bytes; the start-up buffers play at 44.1 kHz.
- Mac OS used the Screamer protocol with the tree naming Burgundy too (a
  real B&W very likely has Burgundy), so the machine's codec is a Screamer.
- No sound heard yet: a Finder stop alert caused no output DMA at all.
- Ethernet: AppleTalk set active fails with "An error occurred attempting
  to use Ethernet" before any access to the cell.

## Checks

- Unit tests 443/443; sabotage runs (pacing, byte swap, mute, tick stop,
  residual, MII turnaround, CRC, own-frame filter, PIC bank, resampler
  step, latency limit, underrun hold, frame length) each fail a test.
- Mac OS 9.2.1 still boots to the Finder from the CD with both cells.
- QEMU raspi4b test build: cores 1-3 start (core 3 with the event
  stream), CPU self-test 20/20 both ways. HDMI sound and Ethernet need real
  hardware (QEMU has neither): untested.

## Later the same day: the web interface

docs/web-interface.md. A page and `/api` served from the Pi: status,
uploads into the image folders, the settings file (checked before saving),
updating the emulator image (with a marker check and a backup) and a
restart. Circle's TCP/IP stack now runs on core 0's scheduler on a stand-in
device; core 3 switches the Ethernet adapter between it and the Mac, who
gets an address derived from the Pi's. Tested on Linux (unit tests,
sabotage, curl, Chromium); not yet run on a Pi.
