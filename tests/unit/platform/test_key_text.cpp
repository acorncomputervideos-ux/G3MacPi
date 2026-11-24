// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_key_text.cpp — characters typed in the menu
// (platform/common/key_text), US and UK layouts.

#include "platform/common/key_text.h"
#include "tests/framework/g3test.h"

using namespace g3;

TEST_CASE("platform/keytext/us_and_uk")
{
    const KeyLayout us = KeyLayout::Us, uk = KeyLayout::Uk;
    CHECK_EQ(key_char(0x04, false, us), 'a');
    CHECK_EQ(key_char(0x1D, true, us), 'Z');
    CHECK_EQ(key_char(0x1E, false, us), '1');
    CHECK_EQ(key_char(0x27, false, us), '0');
    CHECK_EQ(key_char(0x27, true, us), ')');
    CHECK_EQ(key_char(0x2C, false, us), ' ');
    CHECK_EQ(key_char(0x2C, true, us), ' ');
    CHECK_EQ(key_char(0x2D, true, us), '_');
    CHECK_EQ(key_char(0x31, false, us), '\\');
    CHECK_EQ(key_char(0x33, true, us), ':');
    CHECK_EQ(key_char(0x38, true, us), '?');
    // Where US and UK differ.
    CHECK_EQ(key_char(0x1F, true, us), '@');
    CHECK_EQ(key_char(0x1F, true, uk), '"');
    CHECK_EQ(key_char(0x34, true, us), '"');
    CHECK_EQ(key_char(0x34, true, uk), '@');
    CHECK_EQ(key_char(0x20, true, us), '#');
    CHECK_EQ(key_char(0x20, true, uk), 0);  // pound sign: not ASCII
    CHECK_EQ(key_char(0x32, false, uk), '#');
    CHECK_EQ(key_char(0x32, true, uk), '~');
    CHECK_EQ(key_char(0x64, false, uk), '\\');
    CHECK_EQ(key_char(0x64, true, uk), '|');
    CHECK_EQ(key_char(0x35, true, us), '~');
    CHECK_EQ(key_char(0x35, true, uk), 0);  // not sign
    // Keys that type nothing.
    CHECK_EQ(key_char(0x28, false, us), 0);  // Enter
    CHECK_EQ(key_char(0x2A, false, us), 0);  // Backspace
    CHECK_EQ(key_char(0x2B, false, us), 0);  // Tab
    CHECK_EQ(key_char(0x00, false, us), 0);
    CHECK_EQ(key_char(0x39, false, us), 0);  // Caps Lock
    CHECK_EQ(key_char(0xE1, false, us), 0);  // Shift itself
    CHECK(shift_held(0x02));
    CHECK(shift_held(0x20));
    CHECK(!shift_held(0x01 | 0x04));
}
