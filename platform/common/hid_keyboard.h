// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/hid_keyboard.h — converts USB HID boot-protocol keyboard
// reports into individual key press/release events.
//
// A boot-protocol report (USB HID 1.11, Appendix B) carries a modifier byte
// (bit 0 LeftCtrl ... bit 7 RightGUI = usages 0xE0..0xE7) and up to six
// usage IDs of currently pressed keys. The Macintosh ADB keyboard instead
// reports individual key-down/key-up transitions, so we diff consecutive
// reports.

#ifndef G3_PLATFORM_COMMON_HID_KEYBOARD_H
#define G3_PLATFORM_COMMON_HID_KEYBOARD_H

#include "core/common/types.h"
#include "platform/common/input_events.h"

namespace g3 {

namespace hid {
constexpr u8 kUsageNone           = 0x00;
constexpr u8 kUsageErrorRollOver  = 0x01;  // too many keys pressed ("phantom" state)
constexpr u8 kUsagePostFail       = 0x02;
constexpr u8 kUsageErrorUndefined = 0x03;
constexpr u8 kUsageDeleteForward  = 0x4C;
constexpr u8 kUsageLeftCtrl       = 0xE0;  // modifiers occupy 0xE0..0xE7
constexpr u8 kUsageLeftAlt        = 0xE2;
constexpr u8 kUsageRightCtrl      = 0xE4;
constexpr u8 kUsageRightAlt       = 0xE6;

// Short human-readable name for logs ("A", "Enter", "LCtrl", ...).
// Returns "?" for usages without a name.
const char* usage_name(u8 usage);
} // namespace hid

class HidKeyboardTracker {
public:
    // 8 modifier changes + 6 releases + 6 presses.
    static constexpr u32 kMaxEventsPerReport = 20;

    // Processes one report. Writes the resulting events to 'out' (which must
    // hold kMaxEventsPerReport entries) and returns how many were written.
    // Releases are emitted before presses. Error/phantom reports (usages
    // 0x01..0x03) are ignored and leave the state unchanged.
    u32 update(u8 modifiers, const u8 keys[6], KeyEvent* out);

    // Emits releases for everything held (e.g. when the keyboard is unplugged)
    // and clears the state. Returns the number of events written.
    u32 release_all(KeyEvent* out);

    bool is_pressed(u8 usage) const;

private:
    u8 modifiers_ = 0;
    u8 keys_[6] = {};
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_HID_KEYBOARD_H
