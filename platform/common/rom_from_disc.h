// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/rom_from_disc.h — the Mac OS ROM file, checked and found.
//
// The Blue & White G3 starts Mac OS from a file, "Mac OS ROM" in the
// System Folder: a CHRP boot file, which begins with "<CHRP-BOOT>" (the
// file in Mac OS 9.2.1 does; Open Firmware runs its boot script). Its
// firmware finds it as the file of type 'tbxi' in the blessed folder of a
// disk ("\\:tbxi"). find_mac_os_rom() does the same on a disc or disk image
// (HFS or HFS Plus, bare or in the first Apple_HFS partition), so a Mac OS
// CD image can supply the ROM file itself.
//
// Freestanding; not reentrant (one shared volume reader).

#ifndef G3_PLATFORM_COMMON_ROM_FROM_DISC_H
#define G3_PLATFORM_COMMON_ROM_FROM_DISC_H

#include "core/common/types.h"
#include "core/storage/block_device.h"

namespace g3 {

// The data starts like a Mac OS ROM file (a CHRP boot file).
bool is_mac_os_rom(const u8* data, u32 size);

// The Mac OS ROM file of a disc or disk image into buf: its size, or 0 if
// there is none (or it is larger than cap, or not a CHRP boot file).
u32 find_mac_os_rom(BlockDevice& disc, u8* buf, u32 cap);

} // namespace g3

#endif // G3_PLATFORM_COMMON_ROM_FROM_DISC_H
