// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_screen_presenter.cpp — drawing the Mac's screen
// from another core: conversion and scaling, redrawing only changed rows,
// palette and mode changes, pause/resume, and two threads at once.

#include "platform/common/screen_presenter.h"
#include "tests/framework/g3test.h"

#include <atomic>
#include <cstring>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace g3;

namespace {

struct Rig {
    std::vector<u8> vram = std::vector<u8>(4u << 20);
    std::vector<u8> shadow = std::vector<u8>(4u << 20);
    std::vector<u8> host;
    ScreenSurface out;
    std::unique_ptr<ScreenPresenter> p;
    ScreenFrame f;

    Rig(u32 out_w, u32 out_h, u32 out_depth)
    {
        host.assign(static_cast<size_t>(out_w) * out_h * (out_depth / 8), 0xEE);
        out.base = host.data();
        out.width = out_w;
        out.height = out_h;
        out.depth = out_depth;
        out.pitch = out_w * (out_depth / 8);
        p = std::make_unique<ScreenPresenter>(shadow.data(), static_cast<u32>(shadow.size()));
    }
    void mode(u32 w, u32 h, u32 depth, u32 pitch)
    {
        f.on = true;
        f.width = w;
        f.height = h;
        f.depth = depth;
        f.pitch = pitch;
        f.vram = vram.data();
        f.vram_size = static_cast<u32>(vram.size());
    }
    u32 px32(u32 x, u32 y) const
    {
        u32 v;
        std::memcpy(&v, &host[static_cast<size_t>(y) * out.pitch + 4 * x], 4);
        return v;
    }
    u16 px16(u32 x, u32 y) const
    {
        u16 v;
        std::memcpy(&v, &host[static_cast<size_t>(y) * out.pitch + 2 * x], 2);
        return v;
    }
};

} // namespace

TEST_CASE("platform/screen/palette_scaled_and_centred")
{
    // 640x480 at 8 bits onto 1280x1000: fills the width, scale 2, picture
    // at (0, 20).
    Rig r(1280, 1000, 32);
    r.mode(640, 480, 8, 640);
    r.f.palette[0] = 0x000000;
    r.f.palette[7] = 0x123456;
    r.f.palette[9] = 0xABCDEF;
    r.vram[0] = 7;                    // pixel (0, 0)
    r.vram[5 * 640 + 3] = 9;          // pixel (3, 5)
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 480u);
    CHECK_EQ(r.px32(0, 20), 0xFF123456u);
    CHECK_EQ(r.px32(1, 21), 0xFF123456u);    // scaled 2x2
    CHECK_EQ(r.px32(2, 20), 0xFF000000u);
    CHECK_EQ(r.px32(6, 20 + 10), 0xFFABCDEFu);
    CHECK_EQ(r.px32(7, 20 + 11), 0xFFABCDEFu);
    CHECK_EQ(r.px32(0, 0), 0xFF000000u);     // border cleared to black
    CHECK_EQ(r.px32(1279, 999), 0xFF000000u);
    CHECK_EQ(r.px32(500, 19), 0xFF000000u);
    CHECK_EQ(r.p->frames_rendered(), 1u);
}

TEST_CASE("platform/screen/only_changed_rows_are_redrawn")
{
    Rig r(640, 480, 32);
    r.mode(640, 480, 8, 640);
    r.f.palette[1] = 0x010203;
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 480u);
    // Nothing changed: nothing drawn.
    CHECK_EQ(r.p->render(r.out), 0u);
    CHECK_EQ(r.p->rows_skipped(), 480u);
    // One pixel changes: one row.
    r.vram[100 * 640 + 50] = 1;
    CHECK_EQ(r.p->render(r.out), 1u);
    CHECK_EQ(r.px32(50, 100), 0xFF010203u);
    // A host pixel we overwrite in an unchanged row stays overwritten: the
    // row really was skipped.
    r.host[0] = 0x55;
    r.vram[200 * 640] = 1;
    CHECK_EQ(r.p->render(r.out), 1u);
    CHECK_EQ(r.host[0], 0x55u);
}

TEST_CASE("platform/screen/palette_change_redraws_everything")
{
    Rig r(640, 480, 32);
    r.mode(640, 480, 8, 640);
    r.p->publish(r.f);
    r.p->render(r.out);
    r.f.palette[0] = 0x00FF00;  // every pixel is index 0
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 480u);
    CHECK_EQ(r.px32(320, 240), 0xFF00FF00u);
    // Direct-colour modes ignore the palette.
    r.mode(640, 480, 32, 640 * 4);
    r.p->publish(r.f);
    r.p->render(r.out);
    r.f.palette[0] = 0x0000FF;
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 0u);
}

TEST_CASE("platform/screen/direct_modes_onto_rgb565")
{
    Rig r(800, 600, 16);
    // 15-bit: pure red 0x7C00, big-endian in video memory.
    r.mode(800, 600, 15, 1600);
    r.vram[0] = 0x7C;
    r.vram[1] = 0x00;
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 600u);
    CHECK_EQ(r.px16(0, 0), 0xF800u);
    CHECK_EQ(r.px16(1, 0), 0x0000u);
    // 32-bit xRGB: green 0x00FF00. A mode change redraws all rows.
    r.mode(800, 600, 32, 3200);
    r.vram[0] = 0;
    r.vram[1] = 0;
    r.vram[2] = 0xFF;
    r.vram[3] = 0;
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 600u);
    CHECK_EQ(r.px16(0, 0), 0x07E0u);
}

TEST_CASE("platform/screen/mode_change_clears_and_recentres")
{
    Rig r(1024, 768, 32);
    r.mode(1024, 768, 8, 1024);
    r.f.palette[0] = 0xFFFFFF;
    r.p->publish(r.f);
    r.p->render(r.out);
    CHECK_EQ(r.px32(0, 0), 0xFFFFFFFFu);
    // Mac OS picks 640x480 at its desktop: it still fills the screen (as a
    // monitor shows it), not a small picture in the middle.
    r.mode(640, 480, 8, 640);
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 480u);
    CHECK_EQ(r.px32(0, 0), 0xFFFFFFFFu);
    CHECK_EQ(r.px32(1023, 767), 0xFFFFFFFFu);
    // A wider shape (800x400): full width, black bands above and below.
    r.mode(800, 400, 8, 800);
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 400u);
    CHECK_EQ(r.px32(0, 127), 0xFF000000u);
    CHECK_EQ(r.px32(0, 128), 0xFFFFFFFFu);
    CHECK_EQ(r.px32(1023, 639), 0xFFFFFFFFu);
    CHECK_EQ(r.px32(1023, 640), 0xFF000000u);
}

TEST_CASE("platform/screen/nothing_to_show")
{
    Rig r(640, 480, 32);
    r.mode(640, 480, 8, 640);
    r.f.palette[0] = 0xFFFFFF;
    r.p->publish(r.f);
    r.p->render(r.out);
    // Display switched off: blanked once, then nothing.
    r.f.on = false;
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 0u);
    CHECK_EQ(r.px32(100, 100), 0xFF000000u);
    // A mode bigger than the host screen: shrunk to fit.
    r.mode(800, 600, 8, 800);
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 600u);
    CHECK_EQ(r.px32(639, 479), 0xFFFFFFFFu);
    // Inconsistent snapshots are refused, not read out of bounds.
    r.mode(640, 480, 8, 640);
    r.f.vram_size = 640 * 479;           // video memory too small for the mode
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 0u);
    r.mode(640, 480, 8, 320);            // pitch shorter than a row
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 0u);
    r.mode(640, 480, 16, 1280);          // unsupported depth
    r.p->publish(r.f);
    CHECK_EQ(r.p->render(r.out), 0u);
    ScreenPresenter tiny(r.shadow.data(), 1000);  // shadow too small
    r.mode(640, 480, 8, 640);
    tiny.publish(r.f);
    CHECK_EQ(tiny.render(r.out), 0u);
}

TEST_CASE("platform/screen/pause_and_resume")
{
    Rig r(640, 480, 32);
    r.mode(640, 480, 8, 640);
    r.p->publish(r.f);
    r.p->render(r.out);
    r.p->pause();
    r.vram[0] = 1;
    CHECK_EQ(r.p->render(r.out), 0u);  // paused: draws nothing
    r.p->resume();
    CHECK_EQ(r.p->render(r.out), 480u);  // then everything (the menu may have drawn over it)
}

TEST_CASE("platform/screen/two_threads")
{
    // The drawing thread renders continuously while this thread changes
    // video memory, modes and palettes and pauses/resumes. After pause()
    // returns, nothing may be drawn until resume().
    Rig r(1280, 1024, 32);
    std::atomic<bool> stop{false};
    std::thread drawer([&] {
        while (!stop.load())
            r.p->render(r.out);
    });
    const u32 modes[3][4] = {{640, 480, 8, 640}, {800, 600, 15, 1600}, {1024, 768, 32, 4096}};
    bool ok = true;
    for (u32 i = 0; i < 300; ++i) {
        const u32* m = modes[i % 3];
        r.mode(m[0], m[1], m[2], m[3]);
        r.f.palette[i & 255] = i * 2654435761u;
        r.vram[(i * 7919) % (640 * 480)] = static_cast<u8>(i);
        r.p->publish(r.f);
        if (i % 10 == 0) {
            r.p->pause();
            const u64 rows = r.p->rows_redrawn();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            if (r.p->rows_redrawn() != rows)
                ok = false;
            r.p->resume();
        }
    }
    stop.store(true);
    drawer.join();
    CHECK(ok);
    CHECK(r.p->frames_rendered() > 0u);
}
