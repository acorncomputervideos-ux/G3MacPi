# Sound: the DAV bus cell (Screamer / Burgundy codec)

`core/devices/macio/davbus.{h,cpp}`; tests `tests/unit/devices/test_davbus.cpp`.

## Facts and sources

| What | Value | Source |
|---|---|---|
| Cell offset in mac-io | 0x14000 | DingusPPC MacIoTwo |
| Registers | 32-bit little-endian, 16 bytes apart: 0x00 sound control, 0x10 codec control, 0x20 codec status, 0x30 clip count, 0x40 byte swap | Linux `sound/ppc/awacs.h` (`struct awacs_regs`) |
| Frame count | 0x50 | DingusPPC awacs.h; Mac OS 9.2.1 reads it (trace, below) |
| Sound control | bits 8-10 rate (44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350 Hz); bit 11 error, bit 12 port change (write 1 to clear); bits 13/14 their interrupt enables | Linux awacs.h; FreeBSD davbusreg.h |
| Screamer codec control | bits 0-11 data, 12-14 register, bit 24 busy | Linux awacs.h/awacs.c |
| Screamer codec registers | 1: bit 7 mutes C (speaker), bit 9 mutes A; 2 (A) and 4 (C): attenuation, right bits 0-3, left 6-9 | Linux awacs.h |
| Screamer codec status | bit 22 valid, 12-15 revision (3), 8-11 manufacturer (1, Crystal), 0-3 jack sense | Linux awacs.h/awacs.c; DingusPPC returns the same bits (its constant is the byte-swapped form) |
| Burgundy access | codec control bits 12-19 address, 20-21 operation (1 read, 2 byte of a 4-byte register, 3 one-byte register), 8-9 byte, 0-7 data; read data in codec status bits 4-11, "extend" (bit 23) set then cleared | Linux `sound/ppc/burgundy.c` |
| Burgundy output enables | register 0x60: 0x80 internal speaker, 0x06 output, 0x18 line out, 0x60 headphones | Linux burgundy.h |
| DMA | output at mac-io + 0x8800, input + 0x8900 | DingusPPC (channels 8, 9 of 0x100) |
| Interrupts | 17 (sound), 8 (out DMA), 9 (in DMA) | a real B&W's Linux boot log; DingusPPC |
| Device tree | `davbus` with a `sound` child; reg/interrupts in that order; no `layout-id` | Linux pmac.c |
| Samples | 16-bit signed stereo, big-endian unless byte swap = 1 | Linux pmac.c |

## Which codec

A real Blue & White G3 very likely has **Burgundy**: FreeBSD's davbus.c
has "B&W G3 wirings" in its Burgundy code, and DingusPPC puts Screamer in
the Beige G3. But Mac OS 9.2.1 drove the codec with the **Screamer**
protocol even with the device tree naming Burgundy (it then polled for
Screamer's "valid" bit for ever). How Mac OS picks its codec driver is not
known yet, so the machine's codec answers as a Screamer; the Burgundy
protocol is modelled and tested for when that is found out.

## What Mac OS 9.2.1 does (traces with `g3_host`, `G3_MMIO_RANGE`)

- Codec: fades both outputs to full attenuation, recalibrates muted
  (register 1 = 0x284), unmutes and fades up; probes register 7.
- Input DMA (0x8900): starts it and busy-waits on its status while it runs.
- Output DMA (0x8800): rings of LOAD_WORD (the frame count register, key 6),
  OUTPUT_LAST of 2048 bytes, STORE_WORD (interrupt), NOP, and the same
  again, then STOP. The LOAD_WORDs read the frame count around each block:
  **the frame count must advance with the samples going out**. The first
  model (frame count counted when DMA data was consumed, a 4 KB FIFO that
  swallowed a block at once) made the codec look dead.

## Model

- The frame count is a clock: it advances with guest time at the selected
  rate (re-based when the rate changes).
- Every 32 frames (kTickFrames) the codec takes the frames of that interval
  from a 64-byte FIFO, which the output DMA refills as it empties, so DMA
  runs at most a FIFO ahead of the codec. The frames, with the codec's
  volume applied (Screamer: attenuation in 1.5 dB steps, **assumed**),
  go to the `AudioSink`.
- Input delivers silence at the same rate (no microphone).

## On the Pi

`platform/common/audio_bridge` moves the frames to core 3 (lock-free
queue), which resamples them to 48 kHz (linear) and keeps Circle's HDMI
sound queue filled (`app/circle/kernel.cpp`, `CDrawCore::ServiceSound`).
Latency is held under 120 ms; when the Mac is slower than real time the
output holds the last sample. Circle drives HDMI 0 only.

## Status

- Unit tests: registers as Linux uses them, playback at the codec rate,
  volume/mute/byte swap/rate, input pacing, the frame-clock probe as Mac OS
  does it, Burgundy registers; sabotage runs fail them.
- Mac OS 9.2.1 boots with the cell, runs its codec set-up and plays its
  (silent) start-up buffers at the right rate. **Not yet heard**: a Finder
  stop alert produced no sound and no output DMA, with the speaker volume
  in PRAM (byte 0x08) at 0 or 7, and with the feature control register
  kept (which fixed Ethernet).
- **What is missing** (found by locating the code that drives the cell: it
  is loaded from the CD, Apple's sound hardware plug-in architecture). The
  plug-in manager (`SndHWPluginMgrLib`, `FindBuiltInSoundNode`) reads a
  description of the hardware from the `sound` node: `device_type`
  (its strings pair it with `soundchip`, and `davbus` with `soundbus`),
  `#-detects`, `#-inputs`, `#-outputs`, `#-features`, `device-id`,
  `icon-id`, `info-id`, `name-id`, `model`, `object-model-version`,
  `sample-rates`, `frame-size`, `sample-precisions`, `AAPL,output-component`,
  `AAPL,input-component`, `AAPL,port-handler-component`, and more. A real
  B&W's Open Firmware creates them; they are not in the Mac OS ROM file
  and no published dump was found. The two `device_type` values alone
  change nothing (tried). **Needed: the `sound` node of a real Blue &
  White G3** (Open Firmware: `dev /pci/pci-bridge/mac-io/davbus/sound
  .properties`, or Linux: `/proc/device-tree/.../davbus/sound`). The
  DAV-bus plug-in on the CD has Burgundy and AWACS register routines.
- The HDMI path is built for the Pi but not yet run on hardware.
