// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/input_events.h — host input events, independent of Circle.
//
// The platform layer converts whatever the host provides (Circle USB HID,
// test scripts) into these events. The emulated Macintosh input devices
// (ADB keyboard/mouse in Milestone 6) consume them.

#ifndef G3_PLATFORM_COMMON_INPUT_EVENTS_H
#define G3_PLATFORM_COMMON_INPUT_EVENTS_H

#include "core/common/types.h"

namespace g3 {

// A key changed state. 'usage' is the USB HID Usage ID from the Keyboard/
// Keypad page (0x07), e.g. 0x04 = 'A', 0xE0 = Left Control.
struct KeyEvent {
    u8 usage;
    bool pressed;
};

// Relative mouse movement. Buttons: bit 0 = left, bit 1 = right, bit 2 = middle.
struct MouseEvent {
    s16 dx;
    s16 dy;
    s8 wheel;
    u8 buttons;
};

struct InputEvent {
    enum class Type : u8 { Key, Mouse };
    Type type;
    union {
        KeyEvent key;
        MouseEvent mouse;
    };

    static InputEvent make_key(const KeyEvent& k)
    {
        InputEvent e{};
        e.type = Type::Key;
        e.key = k;
        return e;
    }
    static InputEvent make_mouse(const MouseEvent& m)
    {
        InputEvent e{};
        e.type = Type::Mouse;
        e.mouse = m;
        return e;
    }
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_INPUT_EVENTS_H
