// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/apple_partition_map.cpp — see apple_partition_map.h.

#include "core/storage/apple_partition_map.h"

#include "core/common/cstring.h"

namespace g3 {

namespace {
u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p)
{
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
void copy_name(char* dst, const u8* src)
{
    u32 i = 0;
    for (; i < 32 && src[i] != 0; ++i)
        dst[i] = static_cast<char>(src[i]);
    dst[i] = '\0';
}
} // namespace

bool ApplePartitionMap::read(BlockDevice& disk)
{
    count_ = 0;
    u8 blk[BlockDevice::kSectorSize];
    if (disk.sector_count() < 2 || !disk.read(0, 1, blk))
        return false;
    if (be16(blk) != 0x4552 || be16(blk + 2) != BlockDevice::kSectorSize)
        return false;
    if (!disk.read(1, 1, blk) || be16(blk) != 0x504D)
        return false;
    u32 entries = be32(blk + 4);
    if (entries == 0)
        return false;
    if (entries > kMaxPartitions)
        entries = kMaxPartitions;
    for (u32 i = 0; i < entries; ++i) {
        if (i > 0 && (1 + i >= disk.sector_count() || !disk.read(1 + i, 1, blk)))
            break;
        if (be16(blk) != 0x504D)
            break;
        ApmPartition& p = parts_[count_];
        p.start = be32(blk + 8);
        p.blocks = be32(blk + 12);
        p.status = be32(blk + 88);
        copy_name(p.name, blk + 16);
        copy_name(p.type, blk + 48);
        ++count_;
    }
    return count_ > 0;
}

const ApmPartition* ApplePartitionMap::partition(u32 n) const
{
    return (n >= 1 && n <= count_) ? &parts_[n - 1] : nullptr;
}

u32 ApplePartitionMap::find_type(const char* type) const
{
    for (u32 i = 0; i < count_; ++i)
        if (cstr::equal(parts_[i].type, type))
            return i + 1;
    return 0;
}

} // namespace g3
