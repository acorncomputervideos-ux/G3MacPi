// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/new_mac_disk.h — a new hard disk that Mac OS 9 mounts.
//
// Mac OS mounts an ATA hard disk only when the disk carries Apple's ATA
// driver in its own partitions, as Drive Setup writes them
// (docs/storage/disk-format.md). The emulator can't include Apple's
// driver, but the user's own Mac OS disk has it. So a new disk is made from
// an existing one ("the source"): its driver descriptor map and partition
// map, and every partition before its first Apple_HFS partition whose type
// begins "Apple_Driver" or is "Apple_Patches" (the drivers and patches
// Drive Setup installs) are copied to the same blocks; then one Apple_HFS
// partition fills the rest of the new disk, from where the source's first
// Apple_HFS partition starts, with an empty HFS volume in it
// (format_hfs_head). Other partitions of the source (its volumes, free
// space) are not copied.
//
// Facts (Inside Macintosh: Devices; big-endian): block 0 is the driver
// descriptor map (sbSig 'ER' @0, sbBlkSize @2, sbBlkCount @4); map entries
// from block 1 (pmSig 'PM' @0, pmMapBlkCnt @4 — the number of entries, the
// same in each —, pmPyPartStart @8, pmPartBlkCnt @12, pmPartName @16,
// pmParType @48, pmDataCnt @84).
//
// Only the start of the disk and its alternate MDB are written: the caller
// writes head() at offset 0 and alt_mdb() at alt_mdb_offset() of a file
// of new_bytes; the rest may hold anything.

#ifndef G3_CORE_STORAGE_NEW_MAC_DISK_H
#define G3_CORE_STORAGE_NEW_MAC_DISK_H

#include "core/common/types.h"
#include "core/storage/block_device.h"
#include "core/storage/disk_format.h"

namespace g3 {

class NewMacDisk {
public:
    static constexpr u32 kMaxSystemBlocks = 32768;  // the copied part: 16 MB at most
    static constexpr u32 kMaxEntries = 63;

    // Reads the source's maps and works out the new disk. False if the
    // source can't be used (why() says) or new_bytes is unusable.
    bool plan(BlockDevice& src, u64 new_bytes);
    const char* why() const { return why_; }

    // After plan(): bytes of the new disk's start to write, and where its
    // HFS partition starts (in 512-byte blocks).
    u64 head_bytes() const { return static_cast<u64>(hfs_start_) * kBlock + hfs_head_; }
    u32 hfs_start() const { return hfs_start_; }
    u64 alt_mdb_offset() const { return new_bytes_ - 2 * kBlock; }

    // Fills head[0, head_bytes()) and alt_mdb[0, 512).
    bool build(BlockDevice& src, const HfsFormatOptions& opt, u8* head, u8* alt_mdb);

private:
    static constexpr u32 kBlock = 512;
    u64 new_bytes_ = 0;
    u32 src_blocks_ = 0;
    u32 hfs_start_ = 0, hfs_entry_ = 0;  // the source's first Apple_HFS partition, its entry
    u32 map_blocks_ = 0;                  // the map partition's size (entries it can hold)
    u32 keep_[kMaxEntries] = {};          // entry numbers copied, in order (the map's own first)
    u32 nkeep_ = 0;
    u64 hfs_head_ = 0;
    const char* why_ = "";
};

} // namespace g3

#endif // G3_CORE_STORAGE_NEW_MAC_DISK_H
