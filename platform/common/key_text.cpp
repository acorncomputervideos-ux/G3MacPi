// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/key_text.cpp — see key_text.h.

#include "platform/common/key_text.h"

namespace g3 {

namespace {
// Usages 0x1E-0x38 (digits, then Enter..Slash), unshifted and shifted, US.
// 0x28-0x2B (Enter, Escape, Backspace, Tab) type nothing.
constexpr char kPlain[] = "1234567890\0\0\0\0 -=[]\\#;'`,./";
constexpr char kShift[] = "!@#$%^&*()\0\0\0\0 _+{}|~:\"~<>?";
} // namespace

char key_char(u8 usage, bool shift, KeyLayout layout)
{
    if (usage >= 0x04 && usage <= 0x1D)  // a-z
        return static_cast<char>((shift ? 'A' : 'a') + (usage - 0x04));
    if (layout == KeyLayout::Uk) {
        if (shift && usage == 0x1F) return '"';
        if (shift && usage == 0x20) return 0;  // pound sign
        if (shift && usage == 0x34) return '@';
        if (shift && usage == 0x35) return 0;  // not sign
    }
    if (usage == 0x64)  // the key beside left Shift on ISO keyboards
        return shift ? '|' : '\\';
    if (usage >= 0x1E && usage <= 0x38)
        return (shift ? kShift : kPlain)[usage - 0x1E];
    return 0;
}

} // namespace g3
