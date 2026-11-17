// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/hfs_volume.h — read-only HFS Plus (Mac OS Extended) and
// HFS (Mac OS Standard) volumes, enough for the firmware to find and load
// Mac OS's boot file.
//
// Facts (docs/storage/hfs.md; big-endian throughout):
// HFS Plus — Apple Technical Note TN1150:
//  - Volume header at byte 1024: 'H+' (version 4) or 'HX' (version 5);
//    blockSize @40, totalBlocks @44, finderInfo[8] @80 ([0] = blessed
//    System Folder), extents file fork @192, catalog file fork @272.
//  - Fork data (80 bytes): logicalSize u64, clumpSize, totalBlocks, then 8
//    extents {startBlock u32, blockCount u32}.
//  - Catalog key {keyLength u16, parentID u32, name: length u16 + UTF-16};
//    records: folder 1 (folderID @8), file 2 (fileID @8, fileType @48,
//    creator @52, data fork @88, resource fork @168), threads 3 and 4.
//  - Extents key {keyLength u16 = 10, forkType, pad, fileID u32,
//    startBlock u32}, 8 extents per record.
//  - HFS wrapper: an HFS MDB whose drEmbedSigWord (0x7C) is 'H+'; the
//    volume starts at drAlBlSt * 512 + drEmbedExtent.startBlock * drAlBlkSiz.
// HFS — Inside Macintosh: Files (offsets cross-checked with Linux's
// struct hfs_mdb / hfs_cat_* declarations, facts only):
//  - Master Directory Block at 1024: 'BD'; drNmAlBlks @18, drAlBlkSiz @20,
//    drAlBlSt @28 (512-byte blocks), drFndrInfo @92 ([0] = blessed
//    folder, TN1150), drXTFlSize @130 + drXTExtRec @134, drCTFlSize @146
//    + drCTExtRec @150; extent records are 3 × {startBlock u16, count u16}.
//  - Allocation block n is at drAlBlSt * 512 + n * drAlBlkSiz.
//  - Catalog key {keyLength u8, reserved, parentID u32, name: length u8 +
//    MacRoman}; index keys have the fixed length maxKeyLength + 1.
//    Records start with a type byte: folder 1 (dirID @6), file 2 (fdType
//    @4, fdCreator @8, fileID @20, data logical/physical EOF @26/@30, rsrc
//    @36/@40, data extents @74, rsrc extents @86), threads 3 and 4.
//  - Extents key {keyLength u8 = 7, forkType, fileID u32, startBlock u16}.
// Both: B-tree node descriptor and header record as in TN1150; catalog
// order is parentID, then name; extents order fileID, forkType, start.
//
// Name lookups compare ASCII letters case-insensitively and other
// characters exactly (see docs/storage/hfs.md for why that suffices).

#ifndef G3_CORE_STORAGE_HFS_VOLUME_H
#define G3_CORE_STORAGE_HFS_VOLUME_H

#include "core/common/types.h"
#include "core/storage/block_device.h"

namespace g3 {

struct HfsFork {
    u64 logical_size = 0;
    u32 total_blocks = 0;
    u32 start[8] = {};
    u32 count[8] = {};
    u32 file_id = 0;
    u8 fork_type = 0;  // 0 data, 0xFF resource
};

struct HfsEntry {
    u32 parent = 0;
    u32 id = 0;          // fileID or folderID
    bool folder = false;
    u32 type = 0, creator = 0;  // Finder info (files)
    HfsFork data, rsrc;
    u16 name[255] = {};  // UTF-16 (HFS Plus) or MacRoman bytes (HFS)
    u32 name_len = 0;
};

class HfsVolume {
public:
    static constexpr u32 kRootFolder = 2;
    static constexpr u32 kMaxNodeSize = 32768;

    bool mount(BlockDevice& dev);
    bool mounted() const { return mounted_; }
    bool classic() const { return classic_; }  // HFS rather than HFS Plus
    bool wrapped() const { return wrapper_offset_ != 0; }
    u64 volume_offset() const { return wrapper_offset_; }
    u32 block_size() const { return block_size_; }
    u32 finder_info(u32 i) const { return finder_info_[i & 7]; }
    u32 blessed_folder() const { return finder_info_[0]; }

    // Entries directly inside folder `parent`.
    bool lookup(u32 parent, const char* name, HfsEntry& out);
    bool find_by_type(u32 parent, u32 type, HfsEntry& out);  // first file of that type
    using Visitor = bool (*)(void* context, const HfsEntry& e);  // false = stop
    bool list(u32 parent, Visitor visit, void* context);

    // Reads fork bytes; returns the count read (0 at the end), -1 on error.
    s64 read(const HfsFork& fork, u64 offset, u8* buf, u32 len);

private:
    struct Tree {
        HfsFork fork;
        u32 node_size = 0, root = 0, first_leaf = 0, depth = 0;
        bool variable_index_keys = false;
        u32 max_key_length = 0;
        u8* node = nullptr;  // buffer for this tree's nodes
        u32 loaded = 0xFFFFFFFFu;
    };

    bool mount_plus(const u8* vh);
    bool mount_classic(const u8* mdb);
    bool read_bytes(u64 pos, u8* buf, u32 len);
    bool open_tree(Tree& t);
    bool load_node(Tree& t, u32 n);
    u32 records(const Tree& t) const;
    const u8* record(const Tree& t, u32 i) const;
    u32 record_limit(const Tree& t, u32 i) const;
    u32 key_size(const Tree& t, const u8* r, bool index) const;  // bytes incl. length field
    bool map_block(const HfsFork& f, u32 file_block, u32& vol_block, u32& run);
    bool overflow_map(const HfsFork& f, u32 file_block, u32& vol_block, u32& run);
    bool catalog_leaf_for(u32 parent, u32& leaf);
    bool parse_entry(const u8* rec, u32 limit, HfsEntry& out) const;
    template <typename Fn> bool scan_folder(u32 parent, Fn fn);

    BlockDevice* dev_ = nullptr;
    bool mounted_ = false;
    bool classic_ = false;
    u64 wrapper_offset_ = 0;  // bytes: start of the HFS Plus volume in a wrapper
    u64 alloc_base_ = 0;      // bytes: where allocation block 0 starts
    u32 block_size_ = 0;
    u32 total_blocks_ = 0;
    u32 finder_info_[8] = {};
    Tree catalog_, extents_;
    u8 sector_[BlockDevice::kSectorSize] = {};
    u8 cat_node_[kMaxNodeSize] = {};
    u8 ext_node_[kMaxNodeSize] = {};
};

} // namespace g3

#endif // G3_CORE_STORAGE_HFS_VOLUME_H
