# Disk image tools

Host-side helpers for getting files out of classic Mac OS disk images,
for example a Mac OS ROM update or an install CD. Nothing from Apple's
images is kept in the repository: `.gitignore` blocks ROMs and disk
images, and the tools only read files you supply.

## `tools/diskimage/ndif_extract.py`

Converts a MacBinary-encoded Disk Copy 6 image (`.img`, or a
self-mounting `.smi`) to a raw volume image.

    tools/diskimage/ndif_extract.py "Mac_OS_ROM_Update_1.0.smi.bin" volume.img

Formats:
- **MacBinary II**: the fixed 128-byte header and fork layout.
- **The resource fork**: from Inside Macintosh, "Resource Manager".
- **The block map ('bcem' resource 128)**: there's no published
  specification. Its layout (a 12-byte chunk table from offset 128: start
  sector, type, offset, length) was read off Apple's Mac OS ROM Update
  1.0 image. It is confirmed by that image only:
  - every chunk expands to exactly its sector span;
  - the result is a valid HFS volume that our reader lists.
- **ADC**: the three codes as described in the script.

Chunk types other than zeros (0x00), stored (0x02), ADC (0x83) and end
(0xFF) are refused, not guessed.

## `g3_hfs` (tools/hfs/hfs_tool.cpp)

Lists and extracts files with the emulator's own HFS / HFS Plus reader
(core/storage/hfs_volume). It accepts a bare volume or a partitioned
disk, in which case it uses the first Apple_HFS partition.

    g3_hfs volume.img ls
    g3_hfs volume.img get "Folder:File" out.bin [rsrc]

On Apple's ROM Update image, a classic HFS volume with 512-byte blocks, it
lists all six files with the right types and sizes. It also extracts the
1,758,986-byte installer archive and the ReadMe. This is the first
volume made by Apple's own tools that our reader has been checked
against.

## Not handled

Apple Installer archives ("Tome" files, type 'idcp' / creator 'kakc'):
- The header is readable: file names, type/creator, and each fork's
  size and offset.
- The contents are compressed with an undocumented scheme.

The Mac OS ROM file in the ROM update is inside such an archive. A
Mac OS 8.6/9 install CD has the file loose in its System Folder.
