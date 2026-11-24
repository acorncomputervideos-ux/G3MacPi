// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_frame_copy.cpp — off-screen menu drawing
// (platform/common/frame_copy): only changed rows reach the screen, and
// the screen then shows exactly the new picture.

#include "platform/common/frame_copy.h"
#include "platform/common/menu_painter.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

TEST_CASE("platform/framecopy/changed_rows_only")
{
    // 4 rows of 8 bytes; pitches differ (src 8, dst 12 with a guard tail).
    u8 src[4 * 8], dst[4 * 12];
    std::memset(src, 1, sizeof src);
    std::memset(dst, 1, sizeof dst);
    for (u32 y = 0; y < 4; ++y)
        dst[y * 12 + 8] = dst[y * 12 + 9] = dst[y * 12 + 10] = dst[y * 12 + 11] = 0xEE;
    CHECK_EQ(copy_changed_rows(src, 8, dst, 12, 8, 4), 0u);  // nothing differs
    src[1 * 8 + 7] = 5;   // last byte of row 1
    src[3 * 8 + 0] = 6;   // first byte of row 3
    CHECK_EQ(copy_changed_rows(src, 8, dst, 12, 8, 4), 2u);
    CHECK_EQ(dst[1 * 12 + 7], 5);
    CHECK_EQ(dst[3 * 12 + 0], 6);
    for (u32 y = 0; y < 4; ++y)
        CHECK_EQ(dst[y * 12 + 8], 0xEE);  // nothing past row_bytes
    CHECK_EQ(copy_changed_rows(src, 8, dst, 12, 8, 4), 0u);
}

namespace {
struct NoFiles final : DirectoryLister {
    s32 list(const char*, DirEntry*, u32) override { return 0; }
};
} // namespace

TEST_CASE("platform/framecopy/menu_selection_move_touches_few_rows")
{
    constexpr u32 W = 1280, H = 720;
    MachineConfig cfg;
    NoFiles fs;
    ConfigMenu menu(cfg, fs, "/d", "/r");
    ConfigMenu::Screen scr;
    std::vector<u32> back(W * H), front(W * H, 0), fresh(W * H);
    ScreenSurface b{reinterpret_cast<u8*>(back.data()), W, H, W * 4, 32};
    menu.render(scr);
    MenuPainter::paint(scr, b);
    CHECK_EQ(copy_changed_rows(b.base, W * 4, reinterpret_cast<u8*>(front.data()), W * 4, W * 4, H), H);

    menu.key(menu_key::kDown);
    menu.render(scr);
    MenuPainter::paint(scr, b);
    u32 n = copy_changed_rows(b.base, W * 4, reinterpret_cast<u8*>(front.data()), W * 4, W * 4, H);
    CHECK(n > 0);
    CHECK(n <= 2 * ui::kLineHeight + 2);  // the old and the new selected row
    // The screen now matches a fresh paint exactly.
    ScreenSurface f{reinterpret_cast<u8*>(fresh.data()), W, H, W * 4, 32};
    MenuPainter::paint(scr, f);
    CHECK(fresh == front);
}
