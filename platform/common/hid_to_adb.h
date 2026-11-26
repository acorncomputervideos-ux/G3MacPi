// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/hid_to_adb.h — translate USB HID keyboard usages (what the
// Pi's USB keyboard reports, page 0x07) into ADB key codes (what the
// emulated Macintosh keyboard sends).
//
// ADB key codes: the table in QEMU include/hw/input/adb-keys.h (facts; it
// matches Apple's ADB key layout). HID usages: USB HID Usage Tables, page 7.
// Mapping choices for keys a Mac keyboard lacks are documented in
// docs/hardware/adb.md (e.g. the Windows/GUI key is Command, Alt is Option).

#ifndef G3_PLATFORM_COMMON_HID_TO_ADB_H
#define G3_PLATFORM_COMMON_HID_TO_ADB_H

#include "core/common/types.h"

namespace g3 {

constexpr u8 kAdbNoKey = 0xFF;

// Returns the ADB key code for HID usage 'usage', or kAdbNoKey.
u8 hid_usage_to_adb(u8 usage);

} // namespace g3

#endif // G3_PLATFORM_COMMON_HID_TO_ADB_H
