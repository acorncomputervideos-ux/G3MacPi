// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/bootinfo.h — parser for CHRP "bootinfo" objects,
// the text header of Mac OS's boot file ("Mac OS ROM").
//
// Facts (docs/firmware/bootinfo.md; PowerPC Microprocessor CHRP binding to
// IEEE 1275, revision 1.7, section 3.1.6):
//  - SGML-style text: <CHRP-BOOT> containing <DESCRIPTION>, <OS-NAME>,
//    <BOOT-SCRIPT> and <ICON SIZE=w,h COLOR-SPACE=r,g,b><BITMAP>hex
//    pairs</BITMAP></ICON>, in any order. Defaults: SIZE 64,64,
//    COLOR-SPACE 3,3,2.
//  - Element tags and entity names are case-insensitive; other text is
//    case-sensitive. Character set ISO-8859-1.
//  - Unrecognized tags: the material is ignored until their end tag.
//  - Several CHRP-BOOT sections may follow each other (one per boot
//    choice).
//  - Entities &lt; &gt; &amp; stand for < > &. Inside BOOT-SCRIPT also
//    &device; &partition; &directory; &filename; &full-path;, taken from
//    the bootinfo object's own path "&device;:[&partition;][,]\&directory;\&filename;".
//  - The BOOT-SCRIPT is Open Firmware text, run line by line as if typed.
//
// Choice: anything after a section's </CHRP-BOOT> (Mac OS's boot file
// carries binary data there) is not looked at, except to find a following
// section. An unknown entity is kept as written.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_BOOTINFO_H
#define G3_CORE_FIRMWARE_OPENFIRM_BOOTINFO_H

#include "core/common/types.h"

namespace g3::of {

struct BootInfoPath {
    const char* device = "";
    const char* partition = "";
    const char* directory = "";
    const char* filename = "";
};

struct BootInfo {
    static constexpr u32 kMaxText = 256;
    static constexpr u32 kMaxScript = 32 * 1024;
    static constexpr u32 kMaxIconPixels = 64 * 64;

    char description[kMaxText] = {};
    char os_name[kMaxText] = {};
    char script[kMaxScript] = {};
    u32 script_len = 0;
    bool has_script = false;
    bool has_icon = false;
    u32 icon_width = 64, icon_height = 64;
    u32 icon_bits[3] = {3, 3, 2};
    u8 icon[kMaxIconPixels] = {};
    u32 icon_pixels = 0;      // hex pairs actually read
    u32 body_offset = 0;      // byte after this section's </CHRP-BOOT>
};

// True if `data` starts like a bootinfo object (after optional white space).
bool is_bootinfo(const u8* data, u32 len);

// Parses section `index` (0 = first). False if there is no such
// well-formed section, or an element doesn't fit its buffer.
bool parse_bootinfo(const u8* data, u32 len, const BootInfoPath& path, BootInfo& out, u32 index = 0);

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_BOOTINFO_H
