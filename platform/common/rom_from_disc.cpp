// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/rom_from_disc.cpp — see rom_from_disc.h.

#include "platform/common/rom_from_disc.h"

#include "core/storage/apple_partition_map.h"
#include "core/storage/hfs_volume.h"

namespace g3 {

bool is_mac_os_rom(const u8* data, u32 size)
{
    static const char kTag[] = "<CHRP-BOOT>";
    if (!data || size < sizeof kTag - 1)
        return false;
    for (u32 i = 0; i < sizeof kTag - 1; ++i)
        if (data[i] != static_cast<u8>(kTag[i]))
            return false;
    return true;
}

namespace {
ApplePartitionMap g_map;
HfsVolume g_volume;
HfsEntry g_entry;
constexpr u32 kTbxi = (u32('t') << 24) | (u32('b') << 16) | (u32('x') << 8) | u32('i');

// Finds the blessed folder's 'tbxi' file of the disc's volume (the first
// Apple_HFS partition, or the whole image) and, with buf, reads it there
// (while the partition window exists). Returns its size, 0 if none.
u64 tbxi(BlockDevice& disc, u8* buf, u32 cap)
{
    BlockWindow window(disc, 0, disc.sector_count());
    BlockDevice* dev = &disc;
    if (g_map.read(disc)) {
        const u32 n = g_map.find_type("Apple_HFS");
        if (n) {
            window.set(g_map.partition(n)->start, g_map.partition(n)->blocks);
            dev = &window;
        }
    }
    if (!g_volume.mount(*dev) || g_volume.blessed_folder() == 0)
        return 0;
    if (!g_volume.find_by_type(g_volume.blessed_folder(), kTbxi, g_entry) || g_entry.folder)
        return 0;
    const u64 size = g_entry.data.logical_size;
    if (!buf)
        return size ? size : 1;
    if (size == 0 || size > cap)
        return 0;
    const s64 got = g_volume.read(g_entry.data, 0, buf, static_cast<u32>(size));
    if (got != static_cast<s64>(size) || !is_mac_os_rom(buf, static_cast<u32>(size)))
        return 0;
    return size;
}
} // namespace

bool has_startup_system(BlockDevice& disk) { return tbxi(disk, nullptr, 0) != 0; }

u32 find_mac_os_rom(BlockDevice& disc, u8* buf, u32 cap)
{
    return buf ? static_cast<u32>(tbxi(disc, buf, cap)) : 0;
}

} // namespace g3
