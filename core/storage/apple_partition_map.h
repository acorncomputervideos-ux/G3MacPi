// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/apple_partition_map.h — reads the Apple Partition Map of a
// Macintosh disk image.
//
// Facts (docs/storage/apple-partition-map.md; Inside Macintosh: Devices,
// "SCSI Manager" chapter, Driver Descriptor Map and Partition Map; all
// fields big-endian):
//  - Block 0, Driver Descriptor Map: sbSig 0x4552 ('ER') at 0, sbBlkSize
//    (u16) at 2, sbBlkCount (u32) at 4.
//  - Partition map entries in blocks 1, 2, ...: pmSig 0x504D ('PM') at 0,
//    pmMapBlkCnt (u32, number of map entries) at 4, pmPyPartStart (u32) at
//    8, pmPartBlkCnt (u32) at 12, pmPartName (32 chars) at 16, pmParType
//    (32 chars) at 48, pmLgDataStart (u32) at 80, pmDataCnt (u32) at 84,
//    pmPartStatus (u32) at 88.
//  - Open Firmware numbers partitions by map entry, starting at 1 (entry 1
//    is usually the map itself, "Apple_partition_map") — to verify with
//    Apple's firmware; Linux's mac partition code numbers them the same way.
//
// Map entries and partition positions are in 512-byte blocks. Apple CDs
// declare 2048-byte blocks in the Driver Descriptor Map (sbBlkSize) but keep
// the map in 512-byte entries at 512-byte offsets, with starts and lengths in
// 512-byte units (seen in the Mac OS 9.2.1 install CD: its Apple_HFS
// partition starts at 968, i.e. byte 495616, where the HFS volume is); such
// maps are read too. Other block sizes are refused.

#ifndef G3_CORE_STORAGE_APPLE_PARTITION_MAP_H
#define G3_CORE_STORAGE_APPLE_PARTITION_MAP_H

#include "core/common/types.h"
#include "core/storage/block_device.h"

namespace g3 {

struct ApmPartition {
    u32 start = 0;   // first block
    u32 blocks = 0;  // length in blocks
    u32 status = 0;
    char name[33] = {};
    char type[33] = {};
};

class ApplePartitionMap {
public:
    static constexpr u32 kMaxPartitions = 64;

    // Reads the map; false if the disk has no valid map.
    bool read(BlockDevice& disk);

    u32 count() const { return count_; }
    // Partition n, numbered from 1 like Open Firmware. nullptr if absent.
    const ApmPartition* partition(u32 n) const;
    // First partition (lowest number) of the given type, e.g. "Apple_HFS"; 0 if none.
    u32 find_type(const char* type) const;

private:
    ApmPartition parts_[kMaxPartitions];
    u32 count_ = 0;
};

// A window onto part of another block device (a partition).
class BlockWindow final : public BlockDevice {
public:
    BlockWindow(BlockDevice& base, u64 first, u64 count) : base_(base), first_(first), count_(count) {}
    void set(u64 first, u64 count) { first_ = first; count_ = count; }
    u64 sector_count() const override { return count_; }
    bool read_only() const override { return base_.read_only(); }
    bool read(u64 lba, u32 n, u8* out) override
    {
        return lba <= count_ && n <= count_ - lba && base_.read(first_ + lba, n, out);
    }
    bool write(u64 lba, u32 n, const u8* in) override
    {
        return lba <= count_ && n <= count_ - lba && base_.write(first_ + lba, n, in);
    }
    bool flush() override { return base_.flush(); }

private:
    BlockDevice& base_;
    u64 first_, count_;
};

} // namespace g3

#endif // G3_CORE_STORAGE_APPLE_PARTITION_MAP_H
