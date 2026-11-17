# ATA (IDE) — implementation notes

Code: `core/storage/block_device.h`, `core/devices/ata/ata_bus.*`
(the protocol, independent of any host controller) and
`core/devices/macio/macio_ide.*` (the mac-io ATA cell plus DBDMA).
Tests: `tests/unit/devices/test_ata_bus.cpp`,
`tests/unit/devices/test_macio_ide.cpp`, `tests/unit/machine/test_yosemite.cpp`.

## ATA protocol (AtaBus)

Source: the ATA/ATAPI-5 and ATA/ATAPI-6 standards (T13 1321D, 1410D).

| Fact | Value |
|---|---|
| Command block | 0 data, 1 error/features, 2 sector count, 3 LBA low, 4 LBA mid, 5 LBA high, 6 device, 7 status/command |
| Control block | alternate status (read) / device control (write) |
| Status bits | BSY 0x80, DRDY 0x40, DF 0x20, DSC 0x10, DRQ 0x08, ERR 0x01 |
| Error bits | ABRT 0x04, IDNF 0x10, UNC 0x40 |
| Device control | SRST 0x04, nIEN 0x02 |
| Device register | LBA 0x40, DEV 0x10 (selects device 1) |
| Interrupt acknowledge | reading Status or writing Command clears it; Alternate Status does not |
| Register writes | go to both devices; only the selected device executes and drives status and INTRQ |
| Absent device 1 | device 0 answers status reads for it with 0x00 |
| No device at all | host pull-down on DD7, so status reads with bit 7 clear (we return 0x7F; the other bits float) |
| Signature (ATA device) | count 1, LBA low 1, mid 0, high 0, device 0; error 0x01 = diagnostics passed |
| PIO read | interrupt when each DRQ block is ready; none after the last block |
| PIO write | no interrupt before the first block; one after each block, including the last |
| DMA | one interrupt at the end of the command |
| Sector count 0 | means 256 sectors |
| CHS → LBA | (cyl × heads + head) × sectors + (sector − 1); sector 0 is invalid |

Commands implemented: IDENTIFY DEVICE; READ/WRITE SECTORS; READ/WRITE
MULTIPLE plus SET MULTIPLE MODE (powers of two up to 16); READ/WRITE DMA;
READ VERIFY; SEEK; RECALIBRATE; INITIALIZE DEVICE PARAMETERS; SET
FEATURES (write cache, transfer mode, look-ahead, revert to defaults);
the standby, idle and sleep commands; CHECK POWER MODE; FLUSH CACHE;
EXECUTE DEVICE DIAGNOSTIC. Everything else, including PACKET and
IDENTIFY PACKET DEVICE (this is a disk, not ATAPI), is aborted with ABRT.
The disk is 28-bit LBA only; 48-bit commands are aborted.

Our IDENTIFY data: model "G3MacPi virtual disk"; LBA and DMA
supported; multiword DMA 0-2; PIO 3-4; no Ultra DMA (Linux's Heathrow
timing table has no UDMA modes); ATA-1 to ATA-5; FLUSH CACHE; default
geometry 16 heads × 63 sectors.

Modelling choices: commands complete in zero emulated time, so BSY is only
ever seen during SRST. A write to a read-only image is aborted (ABRT).
After a read error, the LBA registers are not updated to the failing
sector (to do if a guest needs it).

## mac-io ATA cell (MacioIde)

Sources: Linux `drivers/ata/pata_macio.c` and QEMU `hw/ide/macio.c`,
`hw/misc/macio/macio.c` (facts only).

| Fact | Value | Confidence |
|---|---|---|
| Register window | little-endian, taskfile register n at n × 0x10, alternate status / control at 0x160 | Linux `pata_macio` — confirmed |
| Data register | 16/32-bit; the byte stream keeps its order in memory (Linux `ioread16_rep`, no swap; libata then treats IDENTIFY as LE words) | Linux + QEMU agree |
| Timing register | 32-bit at 0x200 (stored, no effect) | Linux `IDE_TIMING_CONFIG` |
| Interrupt register 0x300 | KeyLargo and later only; Linux does not use it on Heathrow, so not modelled | Linux |
| Cells in Paddington | IDE0 at BAR + 0x20000, IDE1 at BAR + 0x21000 | QEMU Old World — **to verify** |
| DBDMA channels | 0x16 (IDE0, BAR + 0x8B00), 0x1A (IDE1, BAR + 0x8D00) | QEMU — **to verify** |
| Interrupts | IDE0 13, IDE0 DMA 2, IDE1 14, IDE1 DMA 3 | QEMU `OLDWORLD_IDE*_IRQ` — **to verify** |
| Linux DMA sequence | descriptors INPUT/OUTPUT_MORE…LAST + STOP; cmdptr; ATA command; RUN. Done when status = RUN, !DEAD, !ACTIVE. Stop: `(RUN|WAKE|DEAD) << 16`, wait for !RUN | Linux — confirmed |

The DBDMA channel's interrupt is a pulse (an edge latched by the PIC); the
ATA INTRQ is a level.

## Which bus holds the boot disk (to verify)

On a real B&W G3 the internal hard disk is believed to sit on a separate
PCI Ultra ATA controller (CMD646), with the CD-ROM and Zip on the mac-io
ATA bus; this needs the device-tree dump to confirm. The emulator puts the
boot disk on the mac-io bus first, because Mac OS 9's ATA driver
supports disks there (as on the Beige G3), and adds the CMD646 later (M9).

## Tests

- Protocol: signature, absent devices, INTRQ follows the selected device,
  IDENTIFY contents and string byte order, PIO read/write interrupt
  sequencing, multiple mode, CHS addressing via INITIALIZE DEVICE
  PARAMETERS, IDNF/ABRT errors, sector count 0 = 256, nIEN, SRST
  mid-transfer, EXECUTE DEVICE DIAGNOSTIC, DMA in odd chunk sizes without
  interrupts mid-transfer, read-only images.
- Cell: PIO IDENTIFY through 16-bit reads with the memory byte order
  checked; 32-bit PIO read and write; Linux's DMA read (two scatter-gather
  segments) and DMA write (RUN set before the command, then the command
  kicks the channel); nIEN through 0x160; timing register.
- Machine: a disk on IDE0 through Paddington's BAR; DMA via channel 0x16
  at BAR + 0x8B00; INTRQ level on PIC input 13; DMA event on input 2.
