// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/common/hid_keyboard.h"

namespace g3 {

namespace {

bool contains(const u8 keys[6], u8 usage)
{
    for (int i = 0; i < 6; ++i)
        if (keys[i] == usage)
            return true;
    return false;
}

bool is_error_report(const u8 keys[6])
{
    for (int i = 0; i < 6; ++i)
        if (keys[i] >= hid::kUsageErrorRollOver && keys[i] <= hid::kUsageErrorUndefined)
            return true;
    return false;
}

} // namespace

u32 HidKeyboardTracker::update(u8 modifiers, const u8 keys[6], KeyEvent* out)
{
    if (is_error_report(keys))
        return 0;

    u32 n = 0;

    // 1. Modifier releases.
    for (unsigned bit = 0; bit < 8; ++bit) {
        const u8 mask = static_cast<u8>(1u << bit);
        if ((modifiers_ & mask) && !(modifiers & mask))
            out[n++] = KeyEvent{static_cast<u8>(hid::kUsageLeftCtrl + bit), false};
    }
    // 2. Key releases.
    for (int i = 0; i < 6; ++i) {
        const u8 k = keys_[i];
        if (k != hid::kUsageNone && !contains(keys, k))
            out[n++] = KeyEvent{k, false};
    }
    // 3. Modifier presses.
    for (unsigned bit = 0; bit < 8; ++bit) {
        const u8 mask = static_cast<u8>(1u << bit);
        if (!(modifiers_ & mask) && (modifiers & mask))
            out[n++] = KeyEvent{static_cast<u8>(hid::kUsageLeftCtrl + bit), true};
    }
    // 4. Key presses (ignoring duplicates within the same report).
    for (int i = 0; i < 6; ++i) {
        const u8 k = keys[i];
        if (k == hid::kUsageNone || contains(keys_, k))
            continue;
        bool duplicate = false;
        for (int j = 0; j < i; ++j)
            if (keys[j] == k)
                duplicate = true;
        if (!duplicate)
            out[n++] = KeyEvent{k, true};
    }

    modifiers_ = modifiers;
    for (int i = 0; i < 6; ++i)
        keys_[i] = keys[i];
    return n;
}

u32 HidKeyboardTracker::release_all(KeyEvent* out)
{
    static const u8 kNoKeys[6] = {};
    return update(0, kNoKeys, out);
}

bool HidKeyboardTracker::is_pressed(u8 usage) const
{
    if (usage >= hid::kUsageLeftCtrl && usage <= hid::kUsageLeftCtrl + 7)
        return (modifiers_ >> (usage - hid::kUsageLeftCtrl)) & 1u;
    return usage != hid::kUsageNone && contains(keys_, usage);
}

const char* hid::usage_name(u8 usage)
{
    // Letters and digits (HID Usage Tables, Keyboard/Keypad page 0x07).
    static const char* const kLetters[26] = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char* const kDigits[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static const char* const k28to38[] = {
        "Enter", "Escape", "Backspace", "Tab", "Space", "-", "=", "[", "]", "\\",
        "NonUS#", ";", "'", "`", ",", ".", "/", "CapsLock"};
    static const char* const kF[12] = {"F1", "F2", "F3", "F4",  "F5",  "F6",
                                        "F7", "F8", "F9", "F10", "F11", "F12"};
    static const char* const k46to52[] = {
        "PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp",
        "Delete", "End", "PageDown", "Right", "Left", "Down", "Up"};
    static const char* const kMods[8] = {"LCtrl", "LShift", "LAlt", "LGUI",
                                          "RCtrl", "RShift", "RAlt", "RGUI"};

    if (usage >= 0x04 && usage <= 0x1D) return kLetters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x27) return kDigits[usage - 0x1E];
    if (usage >= 0x28 && usage <= 0x39) return k28to38[usage - 0x28];
    if (usage >= 0x3A && usage <= 0x45) return kF[usage - 0x3A];
    if (usage >= 0x46 && usage <= 0x52) return k46to52[usage - 0x46];
    if (usage >= 0xE0 && usage <= 0xE7) return kMods[usage - 0xE0];
    if (usage == 0x53) return "NumLock";
    if (usage == 0x64) return "NonUS\\";
    if (usage == 0x65) return "Menu";
    return "?";
}

} // namespace g3
