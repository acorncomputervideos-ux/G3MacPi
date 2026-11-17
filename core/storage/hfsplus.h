// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/hfsplus.h — read-only HFS Plus (Mac OS Extended) volumes,
// enough for the firmware to find and load Mac OS's boot file.
//
// Facts (docs/storage/hfsplus.md; Apple Technical Note TN1150 "HFS Plus
// Volume Format"; big-endian throughout):
//  - Volume header at byte 1024 of the volume: signature 'H+' (version 4)
//    or 'HX' (version 5); blockSize at 40; finderInfo[8] at 80
//    (finderInfo[0] = the blessed System Folder's directory ID); fork
//    data of the extents file at 192 and of the catalog file at 272.
//  - Fork data (80 bytes): logicalSize u64, clumpSize, totalBlocks, then 8
//    extents {startBlock, blockCount} in allocation blocks.
//  - B-trees: 14-byte node descriptor {fLink, bLink, kind (-1 leaf, 0
//    index, 1 header), height, numRecords}; record offsets as u16 from the
//    end of the node; header record (node 0) with rootNode, firstLeafNode,
//    nodeSize, maxKeyLength, attributes (kBTVariableIndexKeysMask 4).
//  - Catalog keys {keyLength u16, parentID u32, name: length u16 +
//    UTF-16}, sorted by parentID then name. Records: folder 1, file 2,
//    folder thread 3, file thread 4. File: fileID at 8, Finder fileType at
//    48 and creator at 52, data fork at 88, resource fork at 168.
//    Folder: folderID at 8.
//  - Extents overflow keys {keyLength 10, forkType (0 data, 0xFF
//    resource), pad, fileID, startBlock}, compared by fileID, forkType,
//    startBlock; each record holds 8 more extents.
//  - HFS wrapper: an HFS MDB ('BD') at 1024 whose drEmbedSigWord (0x7C) is
//    'H+'; the embedded volume starts at drAlBlSt * 512 +
//    drEmbedExtent.startBlock * drAlBlkSiz (TN1150, wrapper section).
//
// Name lookups compare ASCII letters case-insensitively and other
// characters exactly (TN1150's full case folding table is not needed to
// find the boot file; it would be for writing).

#ifndef G3_CORE_STORAGE_HFSPLUS_H
#define G3_CORE_STORAGE_HFSPLUS_H

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
    u16 name[255] = {};  // UTF-16
    u32 name_len = 0;
};

class HfsPlusVolume {
public:
    static constexpr u32 kRootFolder = 2;
    static constexpr u32 kMaxNodeSize = 32768;

    bool mount(BlockDevice& dev);
    bool mounted() const { return mounted_; }
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

    bool read_bytes(u64 pos, u8* buf, u32 len);
    bool open_tree(Tree& t, u32 file_id, const u8* fork_data);
    bool load_node(Tree& t, u32 n);
    u32 records(const Tree& t) const;
    const u8* record(const Tree& t, u32 i) const;
    u32 record_limit(const Tree& t, u32 i) const;
    bool map_block(const HfsFork& f, u32 file_block, u32& vol_block, u32& run);
    bool overflow_map(const HfsFork& f, u32 file_block, u32& vol_block, u32& run);
    // Leaf node to start scanning for the first catalog record with this parentID.
    bool catalog_leaf_for(u32 parent, u32& leaf);
    bool parse_entry(const u8* rec, u32 limit, HfsEntry& out) const;
    template <typename Fn> bool scan_folder(u32 parent, Fn fn);

    BlockDevice* dev_ = nullptr;
    bool mounted_ = false;
    u64 wrapper_offset_ = 0;  // bytes
    u32 block_size_ = 0;
    u32 total_blocks_ = 0;
    u32 finder_info_[8] = {};
    Tree catalog_, extents_;
    u8 sector_[BlockDevice::kSectorSize] = {};
    u8 cat_node_[kMaxNodeSize] = {};
    u8 ext_node_[kMaxNodeSize] = {};
};

} // namespace g3

#endif // G3_CORE_STORAGE_HFSPLUS_H
