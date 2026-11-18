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
//  - Old World machines keep XPRAM at 0x1300 and Name Registry data at
//    0x1400; Apple's Mac OS ROM loader falls back to 0x1400 too. Our layout
//    keeps those offsets.
// "APL,MacOS75"'s signature is 0xA0 (NVRAM_SIG_OS): Apple's Mac OS ROM
// loader, finding a partition of that name with another signature, made its
// own with 0xA0 (and the same 0x510 length). Our choices (to verify against a
// real B&W): the order of the partitions, "common" from 0x1800.

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
constexpr u32 kMacOsHeader = 0x12F0, kMacOsLength = 0x510;   // XPRAM 0x1300, Name Registry 0x1400
constexpr u32 kCommonHeader = 0x1800, kCommonLength = 0x800;
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
