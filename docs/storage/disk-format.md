# New disk images (core/storage/disk_format, g3_mkdisk)

`format_apm_hfs` writes an empty Mac disk: a driver descriptor map, an
Apple partition map (the map itself and one `Apple_HFS` partition from
block 64) and an empty HFS (Mac OS Standard) volume. `format_hfs` writes
the bare volume. `g3_mkdisk <image> <MB> [name]` does it from the command
line.

Sources: Inside Macintosh: Devices (driver descriptor map, partition
map) and Inside Macintosh: Files (MDB, volume bitmap, B-trees, catalog
records). Field offsets are listed in `disk_format.h`.

Tests (`tests/unit/storage/test_disk_format.cpp`): fields worked out by
hand, the catalog leaf checked byte by byte, and the disk read back with
our partition-map and HFS readers. Not yet cross-checked against Apple's
tools or another formatter.

## What Mac OS does with such a disk

Attached to the emulated mac-io ATA channel (probe: `G3_DISK`), Mac OS 9's
ROM identifies the drive and reads block 0, the driver descriptor map.
Finding no driver listed, it stops: it mounts an ATA hard disk only when
the disk carries its own ATA driver in an `Apple_Driver_ATA` partition, as
Apple's Drive Setup writes when it initialises a disk. (Seen in the ROM's
68K code, which matches `Apple_Driver_ATA` partitions, names beginning
with "Maci" and the driver start block from the descriptor map.)

So these images are useful for the firmware and for tests. A disk that
Mac OS mounts needs Apple's driver, which the emulator cannot ship; but
the user's own Mac OS disk carries it. `NewMacDisk`
(core/storage/new_mac_disk.h) makes a new disk from one: its driver
descriptor map, partition map and the Apple_Driver*/Apple_Patches
partitions before its first volume are copied to the same blocks, and one
Apple_HFS partition with an empty volume fills the rest. Only the start
and the alternate MDB are written, so a 4 GB disk needs about 9 MB of
memory. Mac OS 9.2.1 (g3_host, a disk Drive Setup initialised as the
source) shows such a 512 MB disk on the desktop as "New Disk", on the CD
bus slave and on the hard disk bus slave. The Pi's "New blank disk" and
`g3_mkdisk --drivers-from <Mac OS disk>` use it.
