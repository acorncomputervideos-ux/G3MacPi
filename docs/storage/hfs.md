# HFS Plus and HFS — implementation notes

Code: `core/storage/hfs_volume.*` (class `HfsVolume`, read-only, both
formats). Tests: `tests/unit/storage/test_hfs.cpp` (images from
`tests/unit/storage/hfs_image.h`).

The firmware needs the file system to find Mac OS's boot file, the
`Mac OS ROM` file (Finder type `tbxi`) in the blessed System Folder.
Open Firmware names it `hd:N,\\:tbxi`.

## Facts

Source: Apple Technical Note TN1150, "HFS Plus Volume Format", read
directly. HFS wrapper master directory block offsets were cross-checked
with Linux's `struct hfs_mdb` (`include/linux/hfs_common.h`), facts only.

| Fact | Value |
|---|---|
| Volume header | byte 1024 of the volume; `'H+'` version 4 or `'HX'` version 5 |
| Header fields used | blockSize @40, totalBlocks @44, finderInfo[8] @80, extentsFile fork @192, catalogFile fork @272 |
| finderInfo[0] | directory ID of the blessed system folder (Mac OS 8/9: the System Folder); [3] Mac OS 8/9 System Folder; [5] Mac OS X |
| Fork data (80 bytes) | logicalSize u64, clumpSize, totalBlocks, 8 × {startBlock, blockCount} |
| Reserved CNIDs | 1 root parent, 2 root folder, 3 extents file, 4 catalog file |
| B-tree node | descriptor {fLink, bLink, kind (−1 leaf, 0 index, 1 header), height, numRecords, reserved} = 14 bytes; record offsets (u16) from the end of the node, one extra entry = start of free space |
| Header record | treeDepth @0, rootNode @2, firstLeafNode @10, nodeSize @18, maxKeyLength @20, keyCompareType @37, attributes @38 (BigKeys 2, VariableIndexKeys 4) |
| Keyed records | key length u16 first; data follows the key, 2-byte aligned. Index keys use keyLength if VariableIndexKeys is set, otherwise maxKeyLength |
| Catalog key | keyLength, parentID u32, name (length u16 + UTF-16); order: parentID, then name (case-insensitive on HFS+) |
| Catalog records | folder 1 (folderID @8, 88 bytes), file 2 (fileID @8, fileType @48, creator @52, data fork @88, resource fork @168, 248 bytes), folder thread 3, file thread 4 |
| Extents overflow key | keyLength 10, forkType (0 data / 0xFF resource), pad, fileID, startBlock; order fileID, forkType, startBlock; record = 8 extents |
| HFS wrapper | MDB `'BD'` at 1024 with drEmbedSigWord `'H+'` @0x7C; embedded volume at drAlBlSt × 512 + drEmbedExtent.startBlock × drAlBlkSiz (drAlBlkSiz @20, drAlBlSt @28, drEmbedExtent @0x7E) |

## Classic HFS (Mac OS Standard)

Source: *Inside Macintosh: Files* (HFS on-disk structures), with every
offset cross-checked against Linux's `struct hfs_mdb`, `hfs_cat_key`,
`hfs_cat_file`, `hfs_cat_dir`, `hfs_ext_key` declarations
(`include/linux/hfs_common.h`, facts only). The blessed folder (first
long of drFndrInfo) is stated in TN1150's wrapper section.

| Fact | Value |
|---|---|
| Master Directory Block | byte 1024, `'BD'`; drNmAlBlks @18, drAlBlkSiz @20 (any multiple of 512), drAlBlSt @28 (in 512-byte sectors), drFndrInfo[8] @92, drXTFlSize @130 + drXTExtRec @134, drCTFlSize @146 + drCTExtRec @150 |
| Allocation block n | at drAlBlSt × 512 + n × drAlBlkSiz |
| Extent record | 3 × {startBlock u16, blockCount u16} |
| B-trees | same node descriptor and header record as HFS Plus; nodes are 512 bytes |
| Catalog key | {keyLength u8, reserved, parentID u32 @2, name length u8 @6, MacRoman name @7}; index keys use the fixed size maxKeyLength + 1 (the tree has no variable-index-keys attribute) |
| Folder record (70 bytes) | type byte 1, dirID @6 |
| File record (102 bytes) | type byte 2, fdType @4, fdCreator @8, fileID @20, data logical/physical EOF @26/@30, resource @36/@40, data extents @74, resource extents @86 |
| Thread records | types 3 (folder) and 4 (file) |
| Extents key | {keyLength u8 = 7, forkType @1, fileID @2, startBlock u16 @6} |

A volume with `'BD'` and an embedded `'H+'` signature is an HFS Plus
volume in a wrapper; without it, it is read as classic HFS. Names are
MacRoman bytes; the same ASCII-only case folding applies.

## How lookups work without Unicode case folding

HFS Plus sorts names with a case-insensitive comparison that needs
Apple's case-folding table. To list or search one folder, the reader
does this instead:

1. Walk down from the root. In each index node, take the last record
   whose parentID is *below* the folder's ID (or the first record). That
   child cannot start after the folder's first record.
2. Scan the leaves forward, following fLink, until the parentID goes past
   the folder.
3. Compare names case-insensitively for ASCII letters, exactly for other
   characters.

That is enough to find `System Folder` and `Mac OS ROM`, or a file by
Finder type. A lookup by a name with accented letters in different case
would miss; the full table is only needed if we ever write to volumes.

The extents overflow tree uses only numeric keys and is searched the
same way.

## Limits

- Read-only. Journals are ignored (safe for reading).
- Hard links, aliases and the attributes file are not followed.

## Testing and its limits

The tests build images in the test code. The classic HFS image uses a
1536-byte allocation block (not a power of two), 512-byte nodes,
fixed-size index keys whose length byte holds the real length (so only
the fixed-size rule works), and an extents-overflow record. The HFS Plus
images, from TN1150's layout, have:
- a two-level catalog with the System Folder spanning two leaf nodes;
- a file with 10 extents, 2 of them in the extents overflow file;
- the same volume inside an HFS wrapper;
- rejects: plain HFS, a bad signature, a bad block size.

The image builder and the reader have the same author, so a shared
misreading of TN1150 would go unnoticed. The independent check is to
read images made by other tools (Linux `mkfs.hfsplus` from `hfsprogs`,
or real Mac OS 9 disk images). Installing those tools in this session
was not permitted; adding them to CI, or testing with a real image, is
the next step.
