// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/bw_nvram.h — the partition layout of the Blue & White's
// 8 KB NVRAM, as firmware formats it when it is blank.
//
// Facts (Linux arch/powerpc/platforms/powermac/nvram.c and
// arch/powerpc/include/uapi/asm/nvram.h, read for facts only):
//  - CHRP partition header, 16 bytes: signature, checksum, length (u16,
//    big-endian, in 16-byte units, header included), name (12 bytes,
//    NUL-padded). On NewWorld machines partitions chain from offset 0 by
//    their lengths to the end of NVRAM.
//  - Checksum: the signature plus the length and name bytes, carries folded
//    back into 8 bits (chrp_checksum).
//  - "common" holds Open Firmware's settings (NVRAM_SIG_OF 0x50);
//    "APL,MacOS75" is Mac OS's: XPRAM at +0x10 (256 bytes), Name Registry
//    data at +0x110. Free space: signature 0x7F, name "wwwwwwwwwwww".
//  - A real Blue & White (Linux boot log, PowerMac1,1, published as a
//    GitHub gist by tomari) reports: OF partition at 0x140, XP partition at
//    0x11b0, NR partition at 0x12b0. So "common"'s header is at 0x130 and
//    "APL,MacOS75"'s at 0x11A0. Our layout uses those offsets.
//  - "APL,MacOS75"'s signature is 0xA0 (NVRAM_SIG_OS): Apple's Mac OS ROM
//    loader, finding a partition of that name with another signature, made
//    its own with 0xA0, 0x510 bytes long.
// Our choices (to verify): what fills 0-0x12F (free space here), the length
// of "common" (up to "APL,MacOS75") and free space after "APL,MacOS75".

#ifndef G3_CORE_MACHINE_BW_NVRAM_H
#define G3_CORE_MACHINE_BW_NVRAM_H

#include "core/common/types.h"
#include "core/devices/macio/nvram.h"

namespace g3 {

struct NvramPartition {
    u32 offset;      // of the header
    u8 signature;
    u32 length;      // bytes, header included
    char name[13];
};

namespace bw_nvram {
constexpr u32 kCommonHeader = 0x130, kCommonLength = 0x11A0 - 0x130;   // data at 0x140
constexpr u32 kMacOsHeader = 0x11A0, kMacOsLength = 0x510;             // XPRAM 0x11B0, Name Registry 0x12B0
constexpr u8 kSigFree = 0x7F, kSigOf = 0x50, kSigMacOs = 0xA0;

u8 checksum(const u8* header);
// True if partitions chain validly (checksums, lengths) from 0 to the end.
bool valid(const MacioNvram& nv);
// Reads partition i of a valid layout; false past the last one.
bool partition(const MacioNvram& nv, u32 i, NvramPartition& out);
// Writes the layout above (all data zero).
void format(MacioNvram& nv);
} // namespace bw_nvram

} // namespace g3

#endif // G3_CORE_MACHINE_BW_NVRAM_H
