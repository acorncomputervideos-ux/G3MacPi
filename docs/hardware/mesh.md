# MESH SCSI cell — implementation notes

Code: `core/devices/macio/mesh.*`. Tests: `tests/unit/devices/test_mesh.cpp`.

A real Blue & White has a MESH SCSI controller in mac-io. A published
Linux boot log of a PowerMac1,1 shows "mesh: configured for synchronous
5 MB/s" on interrupt 12 (DMA interrupt 0). Mac OS's start-up reads its
registers. Before we modelled it, the first read (bus status, mac-io +
0x10040) was a machine check that stopped the NanoKernel.

## Facts

- **Linux `drivers/scsi/mesh.h`** (read for facts only):
  - 16 byte registers 0x10 apart (layout below);
  - the sequence commands and their flags;
  - the exception, error and interrupt bits;
  - 1s clear the exception, error and interrupt bits.
- **Linux `mesh.c`:**
  - its initialisation (reset, mask 7, source ID, timeout 25, async);
  - bus reset through bus status 1 bit 0x80;
  - a selection timeout means no target.
- **DingusPPC** (read for facts): the MESH ID of the cell inside Heathrow
  is 4. Paddington derives from Heathrow (to verify).

| Offset | Register | Offset | Register |
|---|---|---|---|
| 0x00 | count lo | 0x80 | error |
| 0x10 | count hi | 0x90 | interrupt mask |
| 0x20 | FIFO | 0xA0 | interrupt |
| 0x30 | sequence | 0xB0 | source ID |
| 0x40 | bus status 0 | 0xC0 | destination ID |
| 0x50 | bus status 1 | 0xD0 | sync parameters |
| 0x60 | FIFO count | 0xE0 | MESH ID (4) |
| 0x70 | exception | 0xF0 | selection timeout |

## Model: an empty SCSI bus

- Reset, flush FIFO, arbitrate (we win), bus free, and the parity and
  reselection commands all complete at once ("command done").
- Select: nobody answers, so exception "selection timeout" and the
  exception and command-done interrupts.
- Bus-phase commands without a connection give a sequence error.
- The interrupt line is `interrupt & mask`, on PIC input 12. The device
  tree's `scsi` node names it (bw-device-tree.md).

Not modelled yet: SCSI devices, DMA (DBDMA channel 0), and real timing
(a selection times out at once, not after sel_timeout × 10 ms).
