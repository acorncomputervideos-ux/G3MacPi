// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/key_text.h — the character a key types, for text typed
// into the settings menu (the WiFi name and password).
//
// Keys are USB HID usage IDs (HID Usage Tables 1.12, section 10, keyboard
// page); what is printed on them depends on the keyboard's layout. Two
// layouts: US and UK (the Pi 400 is sold with both). They differ in a few
// shifted symbols: Shift+2 is @ (US) or " (UK), Shift+' is " (US) or @
// (UK), Shift+3 is # (US) or a pound sign (UK; not ASCII, not typed), and
// the UK keyboard has a # ~ key (usage 0x32) and a \ | key (0x64). Only
// printable ASCII is returned (WiFi passphrases are ASCII: IEEE 802.11i).
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_KEY_TEXT_H
#define G3_PLATFORM_COMMON_KEY_TEXT_H

#include "core/common/types.h"

namespace g3 {

enum class KeyLayout : u8 { Us, Uk };

// The character for `usage` with or without Shift, or 0 if the key types
// no printable ASCII character.
char key_char(u8 usage, bool shift, KeyLayout layout);

// Whether HID modifier bits (the boot report's modifier byte: bit 1 left
// Shift, bit 5 right Shift) hold Shift.
inline bool shift_held(u8 modifiers) { return (modifiers & 0x22) != 0; }

} // namespace g3

#endif // G3_PLATFORM_COMMON_KEY_TEXT_H
