# DBDMA (Descriptor-Based DMA) — implementation notes

Code: `core/devices/macio/dbdma.*`. Tests: `tests/unit/devices/test_dbdma.cpp`,
`tests/unit/machine/test_yosemite.cpp` (mapping inside Paddington).

> **Correction (2026-11-17):** the first version used a 0x100 channel
> stride (16 channels). That value had no source and was wrong. It is now
> 0x80 (32 channels), per QEMU.

DBDMA is Apple's DMA engine in the mac-io chip. Storage (IDE), serial (SCC),
audio and floppy each own one or more channels. Software builds a program of
16-byte descriptors in RAM and the channel executes it.

## Facts

Sources: the Linux header `arch/powerpc/include/asm/dbdma.h` and its users
(register layout, bit values, the `DBDMA_DO_STOP` and `DBDMA_DO_RESET`
sequences), and QEMU `hw/misc/macio/mac_dbdma.c` (behaviour). Facts only;
no code was copied.

| Fact | Value | Confidence |
|---|---|---|
| Placement in mac-io | BAR + 0x08000, 32 channels × 0x80 (window 0x1000) | QEMU (`DBDMA_CHANNEL_SHIFT` 7, Old World layout) — **to verify for Paddington**; consistent with Linux `struct dbdma_regs` (0x40 bytes) |
| Channels used by IDE | 0x16 (IDE0, BAR + 0x8B00), 0x1A (IDE1, BAR + 0x8D00) | QEMU `macio_realize_ide` — **to verify** |
| Register byte order | little-endian (Linux uses `out_le32`/`in_le32`) | Linux — confirmed by usage |
| Registers | 0x00 control, 0x04 status, 0x08 cmdptr_hi, 0x0C cmdptr, 0x10 intr_sel, 0x14 br_sel, 0x18 wait_sel | Linux header |
| Control write | mask in bits 31-16, value in bits 15-0 | Linux header |
| Status bits | RUN 0x8000, PAUSE 0x4000, FLUSH 0x2000, WAKE 0x1000, DEAD 0x0800, ACTIVE 0x0400, BT 0x0100, device status 0x00FF | Linux header |
| Descriptor | req_count:16, command:16, phy_addr:32, cmd_dep:32, res_count:16, xfer_status:16 (all little-endian) | Linux header |
| Commands | OUTPUT_MORE 0, OUTPUT_LAST 1, INPUT_MORE 2, INPUT_LAST 3, STORE_WORD 4, LOAD_WORD 5, NOP 6, STOP 7 (bits 15-12) | Linux header |
| Command fields | key bits 10-8, interrupt bits 5-4, branch bits 3-2, wait bits 1-0 (never/if-set/if-clear/always) | Linux header |
| Select registers | mask bits 19-16, value bits 3-0; condition = (devstat & mask) == (value & mask) | QEMU |
| Clearing RUN | also clears ACTIVE and DEAD | QEMU |
| STOP | channel goes inactive and stays on the STOP descriptor; WAKE resumes from it | Linux append pattern (driver overwrites the STOP, then sets WAKE) |
| Bad key / null address | channel becomes DEAD and interrupts | QEMU — **to verify** |

## Modelling choices

- Transfers take no guest time when the device is ready. A device that
  accepts or delivers fewer bytes than asked leaves the channel on the
  descriptor; it calls `DbdmaChannel::kick()` when ready, and the transfer
  resumes where it stopped.
- A descriptor normally finishes only after the whole transfer (residual
  0). Packet devices end an INPUT_LAST descriptor early: when the device
  delivers less than asked, the channel asks
  `DbdmaStream::dma_input_packet_end()`; true finishes the descriptor with
  the unfilled bytes as the residual count (BMAC+ Ethernet: Linux takes a
  received frame's length from it). Stream devices keep the default
  (false) and the channel waits for more data.
- FLUSH is acknowledged at once, since nothing is buffered; first, if the
  channel is active on a transfer descriptor, that descriptor's res_count
  (bytes not yet transferred) and xfer_status (status with ACTIVE, FLUSH
  clear) are written. Mac OS 9.2.1's SerialDMA does exactly this to find how many
  serial bytes have arrived: it writes FLUSH, waits for the bit to clear,
  then polls the receive descriptor's xfer_status for ACTIVE (seen in its
  code, disassembled from RAM). It recycles a receive buffer only when its
  xfer_status has ACTIVE and not FLUSH, and partly filled (flushed) buffers
  must be recycled, so the written status has FLUSH clear; with FLUSH set
  the four buffers ran out after four flushes and the driver hung. (QEMU's
  mac_dbdma.c sets FLUSH in the status register while a device flush runs.)
  Not confirmed on hardware.
- At most 4096 descriptors run per call, so a NOP that branches to itself
  can't hang the emulator.
- Channel interrupts go to a per-channel callback. The interrupt controller
  inputs are wired when the devices that use the channels are added.
- `cmdptr_hi` reads 0 (32-bit physical addresses only).

- Interrupts: a channel calls its interrupt callback; on Paddington that
  raises the PIC input and leaves it up until software acks it in the PIC
  (docs/hardware/heathrow-pic.md, "DMA interrupt sources").

## Tests

- OUTPUT_MORE/LAST into a device, with the residual and status written back
  and the channel parked on the STOP.
- INPUT_LAST, then the Linux append pattern (overwrite the STOP, then WAKE).
- Interrupt and branch conditions on device-status bits, including the BT bit.
- A wait condition, and a device that is not ready, resumed with `kick()`.
- The Linux `DBDMA_DO_STOP` and `DBDMA_DO_RESET` sequences, and FLUSH
  acknowledge.
- DEAD on a null address; LOAD_WORD/STORE_WORD.
- Inside Paddington: register access through the BAR, and rollback when a
  mapping conflict occurs.
