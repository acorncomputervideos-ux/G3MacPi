# ATAPI CD-ROM drive

Code: `core/devices/ata/atapi_cdrom.*` (packet commands) and the PACKET
protocol in `core/devices/ata/ata_bus.*`. Tests:
`tests/unit/devices/test_atapi_cdrom.cpp` (a host-driver-style rig; sabotage
runs caught each of: unit attention, the byte-9 TOC format, the eject lock,
the byte count limit, per-device signatures).

The disc is a CD image (ISO): 2048-byte blocks, read through a
BlockDevice (four 512-byte sectors per block).

## Sources

- ATA/ATAPI-5 (T13): PACKET devices: signature (LBA mid 0x14, high 0xEB),
  IDENTIFY DEVICE aborted, IDENTIFY PACKET DEVICE, the PACKET command
  phases (Interrupt Reason = sector count register: CoD bit 0, IO bit 1;
  byte count in LBA mid/high), DEVICE RESET, Error register = sense key << 4.
- SPC / MMC as SFF-8020i uses them: command and data formats (see the header).
- DingusPPC's ATAPI CD-ROM (an emulator that boots Mac OS 9 from CD on
  its Blue & White model), read for facts only: status 0 after reset,
  IDENTIFY PACKET DEVICE word 0 = 0x85C0, INQUIRY byte 3 = 0x21, the READ
  TOC format taken from byte 9 when byte 2 is 0 (Apple's driver), and the
  Apple mode page 0x31 (".App"). To verify on hardware.

## Behaviour

| Command | Reply |
|---|---|
| TEST UNIT READY | no disc: 2/3A/00; first after insertion: 6/28/00 |
| REQUEST SENSE | fixed format, 18 bytes; consumes the sense data |
| INQUIRY | type 5 (CD/DVD), removable, "G3MacPi Virtual CD-ROM" |
| READ CAPACITY | last LBA, 2048 |
| READ(10), READ(12), READ CD (user data only) | out of range: 5/21/00 |
| READ TOC | formats 0 and 1, LBA or MSF; one data track (ADR/CTRL 0x14) |
| MODE SENSE(6/10) | pages 0x01, 0x0E, 0x2A, 0x31, 0x3F; others 5/24/00 |
| MODE SELECT(10) | parameters accepted and ignored |
| START STOP UNIT | eject (unless locked: 5/53/02); tells the front end |
| PREVENT ALLOW MEDIUM REMOVAL, SET CD SPEED, GET CONFIGURATION | supported |
| anything else | 5/20/00 |

PIO data comes in blocks of at most the host's byte count limit (rounded
down to even); DMA (Features bit 0) goes through the controller's DMA
engine with one interrupt at the end.

## With Mac OS

With a disc image on ata0.master, Mac OS 9's ROM (heathrow-ata driver)
sends IDENTIFY PACKET DEVICE, then TEST UNIT READY / REQUEST SENSE /
READ(12) of block 0, and then polls (TEST UNIT READY + READ(12) block 0)
while it looks for a start-up disc. A disc without a Mac OS System Folder
is not bootable, so it keeps looking; starting up from a Mac OS install
disc image has not been tried (it needs the user's own disc image).
