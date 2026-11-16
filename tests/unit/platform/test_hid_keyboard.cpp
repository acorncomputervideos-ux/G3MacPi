// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for platform/common/hid_keyboard.*

#include "platform/common/hid_keyboard.h"
#include "tests/framework/g3test.h"

#include <cstring>

using namespace g3;

namespace {
constexpr u8 kA = 0x04, kB = 0x05, kC = 0x06;
constexpr u8 kModLShift = 0x02, kModLCtrl = 0x01, kModLAlt = 0x04;

struct Events {
    KeyEvent e[HidKeyboardTracker::kMaxEventsPerReport];
    u32 n = 0;
};

Events feed(HidKeyboardTracker& t, u8 mods, u8 k0 = 0, u8 k1 = 0, u8 k2 = 0, u8 k3 = 0,
            u8 k4 = 0, u8 k5 = 0)
{
    const u8 keys[6] = {k0, k1, k2, k3, k4, k5};
    Events ev;
    ev.n = t.update(mods, keys, ev.e);
    return ev;
}
} // namespace

TEST_CASE("platform/hid_keyboard/press_and_release")
{
    HidKeyboardTracker t;
    Events ev = feed(t, 0, kA);
    REQUIRE(ev.n == 1);
    CHECK_EQ(ev.e[0].usage, kA);
    CHECK(ev.e[0].pressed);
    CHECK(t.is_pressed(kA));

    ev = feed(t, 0);
    REQUIRE(ev.n == 1);
    CHECK_EQ(ev.e[0].usage, kA);
    CHECK(!ev.e[0].pressed);
    CHECK(!t.is_pressed(kA));
}

TEST_CASE("platform/hid_keyboard/repeated_report_produces_no_events")
{
    // Keyboards resend the same report; that must not generate repeats.
    HidKeyboardTracker t;
    feed(t, 0, kA);
    CHECK_EQ(feed(t, 0, kA).n, 0u);
}

TEST_CASE("platform/hid_keyboard/key_moves_slot")
{
    // Order inside the 6-byte array is not meaningful.
    HidKeyboardTracker t;
    feed(t, 0, kA, kB);
    Events ev = feed(t, 0, kB, kA);
    CHECK_EQ(ev.n, 0u);
}

TEST_CASE("platform/hid_keyboard/release_before_press")
{
    HidKeyboardTracker t;
    feed(t, 0, kA);
    Events ev = feed(t, 0, kB);  // A released, B pressed in the same report
    REQUIRE(ev.n == 2);
    CHECK_EQ(ev.e[0].usage, kA);
    CHECK(!ev.e[0].pressed);
    CHECK_EQ(ev.e[1].usage, kB);
    CHECK(ev.e[1].pressed);
}

TEST_CASE("platform/hid_keyboard/modifiers_map_to_E0_E7")
{
    HidKeyboardTracker t;
    Events ev = feed(t, kModLShift, kC);
    REQUIRE(ev.n == 2);
    CHECK_EQ(ev.e[0].usage, 0xE1u);  // LShift pressed first
    CHECK(ev.e[0].pressed);
    CHECK_EQ(ev.e[1].usage, kC);
    CHECK(t.is_pressed(0xE1));

    ev = feed(t, 0x80);  // LShift up, C up, RGUI down
    REQUIRE(ev.n == 3);
    CHECK_EQ(ev.e[0].usage, 0xE1u);
    CHECK(!ev.e[0].pressed);
    CHECK_EQ(ev.e[1].usage, kC);
    CHECK(!ev.e[1].pressed);
    CHECK_EQ(ev.e[2].usage, 0xE7u);
    CHECK(ev.e[2].pressed);
}

TEST_CASE("platform/hid_keyboard/rollover_error_is_ignored")
{
    HidKeyboardTracker t;
    feed(t, 0, kA);
    Events ev = feed(t, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01);
    CHECK_EQ(ev.n, 0u);
    CHECK(t.is_pressed(kA));  // state kept
}

TEST_CASE("platform/hid_keyboard/duplicate_usage_in_report")
{
    HidKeyboardTracker t;
    Events ev = feed(t, 0, kA, kA);
    CHECK_EQ(ev.n, 1u);
}

TEST_CASE("platform/hid_keyboard/maximum_events")
{
    HidKeyboardTracker t;
    Events ev = feed(t, 0xFF, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09);
    CHECK_EQ(ev.n, 14u);  // 8 modifiers + 6 keys
    ev = feed(t, 0x00, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F);
    CHECK_EQ(ev.n, 20u);  // 8 mod releases + 6 releases + 6 presses
}

TEST_CASE("platform/hid_keyboard/release_all")
{
    HidKeyboardTracker t;
    feed(t, kModLCtrl | kModLAlt, 0x4C);
    KeyEvent out[HidKeyboardTracker::kMaxEventsPerReport];
    CHECK_EQ(t.release_all(out), 3u);
    CHECK(!t.is_pressed(0xE0));
    CHECK(!t.is_pressed(0x4C));
}

TEST_CASE("platform/hid_keyboard/usage_names")
{
    CHECK(std::strcmp(hid::usage_name(0x04), "A") == 0);
    CHECK(std::strcmp(hid::usage_name(0x1D), "Z") == 0);
    CHECK(std::strcmp(hid::usage_name(0x1E), "1") == 0);
    CHECK(std::strcmp(hid::usage_name(0x27), "0") == 0);
    CHECK(std::strcmp(hid::usage_name(0x28), "Enter") == 0);
    CHECK(std::strcmp(hid::usage_name(0x39), "CapsLock") == 0);
    CHECK(std::strcmp(hid::usage_name(0x45), "F12") == 0);
    CHECK(std::strcmp(hid::usage_name(0x4C), "Delete") == 0);
    CHECK(std::strcmp(hid::usage_name(0x52), "Up") == 0);
    CHECK(std::strcmp(hid::usage_name(0xE0), "LCtrl") == 0);
    CHECK(std::strcmp(hid::usage_name(0xE7), "RGUI") == 0);
    CHECK(std::strcmp(hid::usage_name(0x00), "?") == 0);
}
