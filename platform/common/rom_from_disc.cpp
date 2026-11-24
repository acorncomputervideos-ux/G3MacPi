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

u32 find_mac_os_rom(BlockDevice& disc, u8* buf, u32 cap)
{
    // A partitioned disc: the first Apple_HFS partition; else the whole image.
    static ApplePartitionMap map;
    static HfsVolume volume;
    BlockWindow window(disc, 0, disc.sector_count());
    BlockDevice* dev = &disc;
    if (map.read(disc)) {
        const u32 n = map.find_type("Apple_HFS");
        if (n) {
            window.set(map.partition(n)->start, map.partition(n)->blocks);
            dev = &window;
        }
    }
    if (!volume.mount(*dev) || volume.blessed_folder() == 0)
        return 0;
    HfsEntry e;
    const u32 tbxi = (u32('t') << 24) | (u32('b') << 16) | (u32('x') << 8) | u32('i');
    if (!volume.find_by_type(volume.blessed_folder(), tbxi, e) || e.folder)
        return 0;
    const u64 size = e.data.logical_size;
    if (size == 0 || size > cap)
        return 0;
    const s64 got = volume.read(e.data, 0, buf, static_cast<u32>(size));
    if (got != static_cast<s64>(size) || !is_mac_os_rom(buf, static_cast<u32>(size)))
        return 0;
    return static_cast<u32>(size);
}

} // namespace g3
