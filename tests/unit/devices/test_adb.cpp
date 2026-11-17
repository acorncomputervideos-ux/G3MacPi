// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the ADB bus, keyboard and mouse, and the HID->ADB map.

#include "core/devices/adb/adb.h"
#include "platform/common/hid_to_adb.h"
#include "tests/framework/g3test.h"

using namespace g3;

namespace {
constexpr u8 talk(u8 addr, u8 reg) { return static_cast<u8>((addr << 4) | 0x0C | reg); }
constexpr u8 listen(u8 addr, u8 reg) { return static_cast<u8>((addr << 4) | 0x08 | reg); }
} // namespace

TEST_CASE("devices/adb/register3_identity_and_address_change")
{
    AdbBus bus;
    AdbKeyboard kbd;
    AdbMouse mouse;
    REQUIRE(bus.attach(&kbd));
    REQUIRE(bus.attach(&mouse));
    u8 r[8];
    REQUIRE(bus.command(talk(2, 3), nullptr, 0, r) == 2);
    CHECK_EQ(r[0] & 0x0F, 2u);
    CHECK_EQ(r[1], 1u);                    // keyboard handler 1
    REQUIRE(bus.command(talk(3, 3), nullptr, 0, r) == 2);
    CHECK_EQ(r[0] & 0x0F, 3u);
    CHECK_EQ(r[1], 2u);                    // mouse handler 2
    CHECK_EQ(bus.command(talk(5, 3), nullptr, 0, r), 0u);  // nobody at 5

    // Move the mouse to address 9 (handler 0xFE: address change only).
    const u8 move[2] = {0x09, 0xFE};
    bus.command(listen(3, 3), move, 2, r);
    CHECK_EQ(mouse.address(), 9u);
    CHECK_EQ(mouse.handler(), 2u);
    CHECK_EQ(bus.command(talk(3, 3), nullptr, 0, r), 0u);
    // Handler change (to 1, supported) keeps working at the new address.
    const u8 h1[2] = {0x09, 0x01};
    bus.command(listen(9, 3), h1, 2, r);
    CHECK_EQ(mouse.handler(), 1u);
    const u8 h4[2] = {0x09, 0x04};       // handler 4: not supported here
    bus.command(listen(9, 3), h4, 2, r);
    CHECK_EQ(mouse.handler(), 1u);
    // SendReset restores defaults.
    bus.command(0x00, nullptr, 0, r);
    CHECK_EQ(mouse.address(), 3u);
    CHECK_EQ(mouse.handler(), 2u);
}

TEST_CASE("devices/adb/keyboard_register0")
{
    AdbBus bus;
    AdbKeyboard kbd;
    bus.attach(&kbd);
    u8 r[8];
    CHECK_EQ(bus.command(talk(2, 0), nullptr, 0, r), 0u);  // nothing to report
    kbd.key_event(0x00, true);   // A down
    kbd.key_event(0x00, false);  // A up
    kbd.key_event(0x31, true);   // Space down
    REQUIRE(bus.command(talk(2, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[0], 0x00u);
    CHECK_EQ(r[1], 0x80u);       // release has bit 7 set
    REQUIRE(bus.command(talk(2, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[0], 0x31u);
    CHECK_EQ(r[1], 0xFFu);       // no second key
    // Power key: both bytes.
    kbd.key_event(AdbKeyboard::kPowerKey, true);
    REQUIRE(bus.command(talk(2, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[0], 0x7Fu);
    CHECK_EQ(r[1], 0x7Fu);
    // Flush empties the queue.
    kbd.key_event(0x01, true);
    bus.command(0x21, nullptr, 0, r);  // Flush address 2
    CHECK(!kbd.has_data());
}

TEST_CASE("devices/adb/mouse_register0")
{
    AdbBus bus;
    AdbMouse mouse;
    bus.attach(&mouse);
    u8 r[8];
    CHECK_EQ(bus.command(talk(3, 0), nullptr, 0, r), 0u);
    mouse.motion(5, -3);  // right 5, up 3
    REQUIRE(bus.command(talk(3, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[0], 0x80u | (static_cast<u8>(-3) & 0x7F));  // Y first, button up
    CHECK_EQ(r[1], 0x80u | 5u);                              // X second
    CHECK_EQ(bus.command(talk(3, 0), nullptr, 0, r), 0u);   // reported
    // Large motion is clamped to 63 per report and the rest kept.
    mouse.motion(100, 0);
    REQUIRE(bus.command(talk(3, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[1] & 0x7F, 63u);
    REQUIRE(bus.command(talk(3, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[1] & 0x7F, 37u);
    // Button press without motion is reported (bit 7 clear = pressed).
    mouse.buttons(1);
    REQUIRE(bus.command(talk(3, 0), nullptr, 0, r) == 2);
    CHECK_EQ(r[0], 0x00u);
    CHECK_EQ(r[1], 0x80u);
}

TEST_CASE("platform/hid_to_adb/map")
{
    CHECK_EQ(hid_usage_to_adb(0x04), 0x00u);  // A
    CHECK_EQ(hid_usage_to_adb(0x16), 0x01u);  // S
    CHECK_EQ(hid_usage_to_adb(0x1D), 0x06u);  // Z
    CHECK_EQ(hid_usage_to_adb(0x27), 0x1Du);  // 0
    CHECK_EQ(hid_usage_to_adb(0x28), 0x24u);  // Enter -> Return
    CHECK_EQ(hid_usage_to_adb(0x2C), 0x31u);  // Space
    CHECK_EQ(hid_usage_to_adb(0x29), 0x35u);  // Escape
    CHECK_EQ(hid_usage_to_adb(0xE3), 0x37u);  // Left GUI -> Command
    CHECK_EQ(hid_usage_to_adb(0xE2), 0x3Au);  // Left Alt -> Option
    CHECK_EQ(hid_usage_to_adb(0xE1), 0x38u);  // Left Shift
    CHECK_EQ(hid_usage_to_adb(0x52), 0x3Eu);  // Up
    CHECK_EQ(hid_usage_to_adb(0x00), kAdbNoKey);
    // No two HID usages map to the same ADB code, except intentional
    // aliases (both GUI keys -> Command, NonUS# -> backslash).
    int dup = 0;
    for (u32 a = 0; a < 256; ++a)
        for (u32 b = a + 1; b < 256; ++b) {
            const u8 x = hid_usage_to_adb(static_cast<u8>(a));
            if (x != kAdbNoKey && x == hid_usage_to_adb(static_cast<u8>(b)))
                ++dup;
        }
    CHECK_EQ(dup, 2);
}
