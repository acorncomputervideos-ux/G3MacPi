// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_menu_painter.cpp — the settings menu drawn as a
// classic Mac desktop (platform/common/menu_painter): the main parts are
// where they should be, and nothing is drawn outside the surface.

#include "platform/common/menu_painter.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

namespace {

struct NoFiles final : DirectoryLister {
    s32 list(const char*, DirEntry*, u32) override { return 0; }
};

struct Rig {
    MachineConfig cfg;
    NoFiles fs;
    ConfigMenu menu{cfg, fs, "/d", "/r"};
    ConfigMenu::Screen scr;
    std::vector<u32> px;
    ScreenSurface s;
    Rig(u32 w, u32 h, u32 guard = 0) : px(static_cast<size_t>(w) * h + guard, 0x12345678u)
    {
        s.base = reinterpret_cast<u8*>(px.data());
        s.width = w;
        s.height = h;
        s.pitch = w * 4;
        s.depth = 32;
    }
    void paint()
    {
        menu.render(scr);
        MenuPainter::paint(scr, s);
    }
    u32 at(u32 x, u32 y) const { return px[static_cast<size_t>(y) * s.width + x] & 0xFFFFFF; }
};

} // namespace

TEST_CASE("platform/menupaint/font_and_scale")
{
    CHECK(ui::text_width("") == 0);
    CHECK(ui::text_width("Il") < ui::text_width("MW"));  // proportional
    CHECK_EQ(ui::text_width("M"), 8u);                   // 7 columns drawn, emboldened to 8
    CHECK_EQ(MenuPainter::scale_for(1280, 720), 1u);
    CHECK_EQ(MenuPainter::scale_for(1920, 1080), 2u);
    CHECK_EQ(MenuPainter::scale_for(3840, 2160), 4u);
    CHECK_EQ(MenuPainter::scale_for(320, 200), 1u);
}

TEST_CASE("platform/menupaint/desktop_menubar_window")
{
    Rig r(1280, 720);
    r.paint();
    CHECK_EQ(r.at(640, 5), 0xFFFFFFu);    // the menu bar
    CHECK_EQ(r.at(640, 19), 0x000000u);   // its bottom line
    CHECK_EQ(r.at(0, 0), 0x000000u);      // the rounded screen corners
    CHECK_EQ(r.at(1279, 0), 0x000000u);
    // The desktop: a two-colour dither.
    CHECK(r.at(5, 600) != r.at(6, 600));
    CHECK_EQ(r.at(5, 600), r.at(6, 601));
    // The window, centred: a black frame around white, with a shadow.
    u32 left = 0;
    for (u32 x = 0; x < 640; ++x)
        if (r.at(x, 400) == 0x000000u) {
            left = x;
            break;
        }
    CHECK(left > 100 && left < 400);
    CHECK_EQ(r.at(left + 5, 400), 0xFFFFFFu);
    CHECK_EQ(r.at(1280 - left + 1, 400), 0x000000u);  // the right edge and its shadow
    // The selected row (the first item): white text on a black bar.
    u32 black = 0, white = 0;
    for (u32 y = 100; y < 600; ++y)
        for (u32 x = left + 20; x < left + 200; ++x) {
            if (r.at(x, y) != 0x000000u)
                continue;
            // a black run as long as the bar
            bool bar = true;
            for (u32 k = 0; k < 150 && bar; ++k)
                bar = r.at(x + k, y) == 0x000000u || r.at(x + k, y) == 0xFFFFFFu;
            if (bar) {
                for (u32 k = 0; k < 150; ++k)
                    (r.at(x + k, y + 6) == 0x000000u ? black : white) += 1;
                y = 600;
                break;
            }
        }
    CHECK(black > 80);   // mostly bar
    CHECK(white > 5);    // with text in it
    // The alpha byte is set (Circle's 32-bit frame buffer is ARGB).
    CHECK_EQ(r.px[0] >> 24, 0xFFu);
}

TEST_CASE("platform/menupaint/stays_inside_small_and_16bit_surfaces")
{
    // A screen smaller than the window: clipped, and the guard words after
    // the buffer stay untouched.
    Rig r(320, 200, 64);
    r.paint();
    for (u32 i = 0; i < 64; ++i)
        CHECK_EQ(r.px[320 * 200 + i], 0x12345678u);
    // 16-bit (RGB565): white menu bar, nothing written past the end.
    std::vector<u16> p16(800 * 600 + 64, 0xBEEF);
    ScreenSurface s;
    s.base = reinterpret_cast<u8*>(p16.data());
    s.width = 800;
    s.height = 600;
    s.pitch = 1600;
    s.depth = 16;
    MenuPainter::paint(r.scr, s);
    CHECK_EQ(p16[5 * 800 + 400], 0xFFFFu);
    for (u32 i = 0; i < 64; ++i)
        CHECK_EQ(p16[800 * 600 + i], 0xBEEFu);
    // Unsupported depth or no buffer: nothing happens.
    s.depth = 24;
    MenuPainter::paint(r.scr, s);
    s.base = nullptr;
    s.depth = 32;
    MenuPainter::paint(r.scr, s);
}
