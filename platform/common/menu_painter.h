// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/menu_painter.h — draws the settings menu (ConfigMenu's
// text screen) as a classic Macintosh desktop: a dithered grey desktop, a
// white menu bar with rounded screen corners, and a document window with
// a pinstriped title bar, a close box and a drop shadow. The selected row
// is inverted (white on black), unavailable rows are grey, status lines
// sit in a footer. Text uses our own bold bitmap font (ui_font.inc, drawn
// for this project in the spirit of the classic system font).
//
// Layout: the window's rows follow the menu's rows; within a row, text is
// split at runs of two or more spaces and each piece starts at its column
// (kCell pixels per column), so columns line up although the font is
// proportional. Everything is drawn in logical pixels and scaled by the
// largest whole factor that fits the screen.
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_MENU_PAINTER_H
#define G3_PLATFORM_COMMON_MENU_PAINTER_H

#include "core/common/types.h"
#include "platform/common/config_menu.h"
#include "platform/common/screen_presenter.h"

namespace g3 {

namespace ui {
constexpr u32 kFontHeight = 12;  // rows per glyph (baseline under row 8)
constexpr u32 kLineHeight = 16;
// Logical width of text in the menu font.
u32 text_width(const char* text);
}  // namespace ui

class MenuPainter {
public:
    static constexpr u32 kCell = 7;  // logical pixels per menu column

    // Draws the whole screen. The hint shows at the right of the menu bar.
    static void paint(const ConfigMenu::Screen& screen, const ScreenSurface& out,
                      const char* hint = "Arrows: move/change   Enter: choose   Esc: back");
    // The scale paint() uses for this screen size (at least 1).
    static u32 scale_for(u32 width, u32 height);
    // The start-up screen: the desktop pattern, our computer mark large in
    // the middle, `message` under it and `hint` at the bottom ("" for none).
    static void paint_splash(const ScreenSurface& out, const char* message, const char* hint);
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_MENU_PAINTER_H
