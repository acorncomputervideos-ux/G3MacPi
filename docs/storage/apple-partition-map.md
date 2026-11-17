# Apple Partition Map — implementation notes

Code: `core/storage/apple_partition_map.*`. Used by `of::DiskDriver`
for Open Firmware paths such as `hd:9,\\:tbxi`. Tests:
`tests/unit/firmware/test_openfirm.cpp` (`storage/apm/*`,
`firmware/client/open_partition`).

Source: *Inside Macintosh: Devices*, SCSI Manager chapter (Driver
Descriptor Map and Partition Map). All fields are big-endian.

| Structure | Field | Offset | Meaning |
|---|---|---|---|
| Driver Descriptor Map (block 0) | sbSig | 0 | 0x4552 ('ER') |
| | sbBlkSize | 2 | block size (we support 512 only) |
| | sbBlkCount | 4 | blocks on the device |
| Partition map entry (blocks 1…) | pmSig | 0 | 0x504D ('PM') |
| | pmMapBlkCnt | 4 | number of map entries |
| | pmPyPartStart | 8 | first block of the partition |
| | pmPartBlkCnt | 12 | blocks in the partition |
| | pmPartName | 16 | 32 characters |
| | pmParType | 48 | 32 characters, e.g. `Apple_HFS`, `Apple_partition_map`, `Apple_Driver43`, `Apple_Free` |
| | pmLgDataStart / pmDataCnt | 80 / 84 | data area inside the partition |
| | pmPartStatus | 88 | status flags |

Partition numbers: Open Firmware counts map entries from 1. Entry 1 is
normally the map itself. Linux numbers Mac partitions the same way;
Apple's firmware behaviour is to verify.

Choices: an entry that runs past the end of the image is clipped; the
map is read again at each `open` (it is small); reading stops at the
first entry without the `PM` signature; at most 64 entries.
