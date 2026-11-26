// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/new_mac_disk.cpp — see new_mac_disk.h.

#include "core/storage/new_mac_disk.h"

namespace g3 {

namespace {

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be32(const u8* p) { return (static_cast<u32>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void put32(u8* p, u32 v)
{
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}
// The 32-byte name or type field equals `s`, or begins with it (prefix).
bool field_is(const u8* f, const char* s, bool prefix)
{
    u32 i = 0;
    for (; s[i]; ++i)
        if (i >= 32 || f[i] != static_cast<u8>(s[i]))
            return false;
    return prefix || i >= 32 || f[i] == 0;
}

} // namespace

bool NewMacDisk::plan(BlockDevice& src, u64 new_bytes)
{
    why_ = "";
    nkeep_ = 0;
    u8 b[kBlock];
    if (src.sector_count() < 2 || !src.read(0, 1, b) || be16(b) != 0x4552 || be16(b + 2) != kBlock) {
        why_ = "the Mac OS disk has no Apple partition map";
        return false;
    }
    src_blocks_ = src.sector_count() > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(src.sector_count());
    if (!src.read(1, 1, b) || be16(b) != 0x504D) {
        why_ = "the Mac OS disk has no Apple partition map";
        return false;
    }
    const u32 entries = be32(b + 4);
    if (entries == 0 || entries > kMaxEntries) {
        why_ = "the Mac OS disk's partition map is damaged";
        return false;
    }
    // The first Apple_HFS partition, the map's own size.
    hfs_start_ = 0;
    map_blocks_ = 0;
    for (u32 e = 1; e <= entries; ++e) {
        if (!src.read(e, 1, b) || be16(b) != 0x504D) {
            why_ = "the Mac OS disk's partition map is damaged";
            return false;
        }
        if (field_is(b + 48, "Apple_partition_map", false))
            map_blocks_ = be32(b + 12);
        if (!hfs_start_ && field_is(b + 48, "Apple_HFS", false)) {
            hfs_start_ = be32(b + 8);
            hfs_entry_ = e;
        }
    }
    if (!hfs_start_ || !map_blocks_ || hfs_start_ > kMaxSystemBlocks || map_blocks_ > kMaxEntries + 1) {
        why_ = "the Mac OS disk's partition map is not as Drive Setup writes it";
        return false;
    }
    // What to copy: the map, the drivers and patches before the volume.
    bool ata = false;
    for (u32 e = 1; e <= entries; ++e) {
        src.read(e, 1, b);
        const u32 start = be32(b + 8), count = be32(b + 12);
        const bool map = field_is(b + 48, "Apple_partition_map", false);
        const bool driver = field_is(b + 48, "Apple_Driver", true) || field_is(b + 48, "Apple_Patches", false);
        if (!(map || driver) || start + count > hfs_start_)
            continue;
        if (map)  // the map's own entry first
            for (u32 k = nkeep_; k > 0; --k)
                keep_[k] = keep_[k - 1];
        keep_[map ? 0 : nkeep_] = e;
        ++nkeep_;
        ata = ata || field_is(b + 48, "Apple_Driver_ATA", false);
    }
    if (!ata) {
        why_ = "the Mac OS disk has no ATA driver partition (initialise it with Drive Setup)";
        return false;
    }
    if (nkeep_ + 1 > map_blocks_) {
        why_ = "the Mac OS disk's partition map is full";
        return false;
    }
    // The new disk: whole blocks, a volume of 1 MB at least, 32-bit block numbers.
    if (new_bytes % kBlock || new_bytes / kBlock > 0xFFFFFFFFull ||
        new_bytes / kBlock < static_cast<u64>(hfs_start_) + 2048) {
        why_ = "that size can't hold a volume";
        return false;
    }
    new_bytes_ = new_bytes;
    hfs_head_ = hfs_head_bytes(new_bytes - static_cast<u64>(hfs_start_) * kBlock);
    if (!hfs_head_) {
        why_ = "that size can't hold a volume";
        return false;
    }
    return true;
}

bool NewMacDisk::build(BlockDevice& src, const HfsFormatOptions& opt, u8* head, u8* alt_mdb)
{
    // The source's start as it is: maps, drivers, patches (and whatever
    // else lies before its volume, overwritten or left unreferenced).
    for (u32 blk = 0; blk < hfs_start_;) {
        const u32 n = hfs_start_ - blk < 128 ? hfs_start_ - blk : 128;
        if (!src.read(blk, n, head + static_cast<u64>(blk) * kBlock)) {
            why_ = "the Mac OS disk could not be read";
            return false;
        }
        blk += n;
    }
    const u32 total = static_cast<u32>(new_bytes_ / kBlock);
    const u32 hfs_blocks = total - hfs_start_;

    // Driver descriptor map: the disk's size (Drive Setup's convention
    // kept: the source's count relative to its size).
    const u32 src_count = be32(head + 4);
    put32(head + 4, src_count + 1 == src_blocks_ ? total - 1 : total);

    // The partition map: the kept entries, then the new volume's.
    u8 entries[kMaxEntries + 1][kBlock];
    for (u32 k = 0; k < nkeep_; ++k)
        for (u32 i = 0; i < kBlock; ++i)
            entries[k][i] = head[(keep_[k]) * kBlock + i];
    u8* hfs = entries[nkeep_];
    if (!src.read(hfs_entry_, 1, hfs)) {
        why_ = "the Mac OS disk could not be read";
        return false;
    }
    put32(hfs + 8, hfs_start_);
    put32(hfs + 12, hfs_blocks);
    put32(hfs + 80, 0);           // pmLgDataStart
    put32(hfs + 84, hfs_blocks);  // pmDataCnt
    for (u32 i = 0; i < 32; ++i)
        hfs[16 + i] = 0;
    for (u32 i = 0; opt.volume_name && opt.volume_name[i] && i < 31; ++i)
        hfs[16 + i] = static_cast<u8>(opt.volume_name[i]);
    const u32 n = nkeep_ + 1;
    for (u32 blk = 1; blk < 1 + map_blocks_; ++blk)
        for (u32 i = 0; i < kBlock; ++i)
            head[blk * kBlock + i] = 0;
    for (u32 k = 0; k < n; ++k) {
        put32(entries[k] + 4, n);
        for (u32 i = 0; i < kBlock; ++i)
            head[(1 + k) * kBlock + i] = entries[k][i];
    }

    // The volume.
    if (!format_hfs_head(head + static_cast<u64>(hfs_start_) * kBlock, static_cast<u64>(hfs_blocks) * kBlock,
                         opt, alt_mdb)) {
        why_ = "the volume could not be formatted (its name?)";
        return false;
    }
    return true;
}

} // namespace g3
