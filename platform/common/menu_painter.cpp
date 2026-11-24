// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/menu_painter.cpp — see menu_painter.h.

#include "platform/common/menu_painter.h"

namespace g3 {

namespace {

struct Glyph {
    u8 width;
    u16 rows[12];
};
const Glyph kFont[95] = {
#include "platform/common/ui_font.inc"
};

constexpr u32 kBlack = 0x000000, kWhite = 0xFFFFFF, kGrey = 0x8A8A8A, kLine = 0xA0A0A0;
constexpr u32 kDesk1 = 0x7C7C8E, kDesk2 = 0x8E8EA0;  // the desktop dither
constexpr u32 kChanged = 0x00207A;                   // rows changed since start-up
constexpr u32 kMenuBar = 20, kTitleBar = 19, kPad = 10;
constexpr u32 kContentW = ConfigMenu::kCols * MenuPainter::kCell;

const Glyph& glyph(char c)
{
    const u32 i = static_cast<u8>(c) >= 0x20 && static_cast<u8>(c) < 0x7F ? static_cast<u8>(c) - 0x20 : '?' - 0x20;
    return kFont[i];
}

// Logical pixels, scaled onto the surface.
struct Canvas {
    const ScreenSurface& s;
    u32 k;       // scale
    u32 w, h;    // logical size

    void fill(s32 x, s32 y, s32 fw, s32 fh, u32 rgb) const
    {
        s32 x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        s32 x1 = x + fw > static_cast<s32>(w) ? static_cast<s32>(w) : x + fw;
        s32 y1 = y + fh > static_cast<s32>(h) ? static_cast<s32>(h) : y + fh;
        if (x0 >= x1 || y0 >= y1)
            return;
        const u16 c16 = static_cast<u16>(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
        for (u32 py = static_cast<u32>(y0) * k; py < static_cast<u32>(y1) * k; ++py) {
            u8* row = s.base + static_cast<size_t>(py) * s.pitch;
            if (s.depth == 32) {
                u32* p = reinterpret_cast<u32*>(row);
                for (u32 px = static_cast<u32>(x0) * k; px < static_cast<u32>(x1) * k; ++px)
                    p[px] = rgb | 0xFF000000u;
            } else {
                u16* p = reinterpret_cast<u16*>(row);
                for (u32 px = static_cast<u32>(x0) * k; px < static_cast<u32>(x1) * k; ++px)
                    p[px] = c16;
            }
        }
    }
    void dot(s32 x, s32 y, u32 rgb) const { fill(x, y, 1, 1, rgb); }

    // Text with its line's top at y (glyphs start 2 pixels lower); stops
    // before max_x. Returns the x after the text.
    s32 text(s32 x, s32 y, const char* t, u32 rgb, s32 max_x = 1 << 30) const
    {
        for (; *t; ++t) {
            const Glyph& g = glyph(*t);
            if (x + g.width > max_x)
                break;
            for (u32 r = 0; r < ui::kFontHeight; ++r)
                for (u32 c = 0; c < g.width; ++c)
                    if (g.rows[r] & (1u << c))
                        dot(x + static_cast<s32>(c), y + 2 + static_cast<s32>(r), rgb);
            x += g.width + 1;
        }
        return x;
    }
};

bool empty(const char* t)
{
    for (; *t; ++t)
        if (*t != ' ')
            return false;
    return true;
}

// The row's text without leading and trailing blanks, into buf.
void trimmed(const char* t, char* buf, u32 cap)
{
    while (*t == ' ')
        ++t;
    u32 n = 0;
    for (; t[n] && n + 1 < cap; ++n)
        buf[n] = t[n];
    while (n && buf[n - 1] == ' ')
        --n;
    buf[n] = 0;
}

// Draws a row: pieces separated by two or more blanks start at their column.
void row_text(const Canvas& c, s32 left, s32 y, const char* t, u32 rgb, s32 max_x)
{
    char piece[ConfigMenu::kCols + 1];
    u32 i = 0;
    while (t[i]) {
        while (t[i] == ' ')
            ++i;
        if (!t[i])
            break;
        const u32 col = i;
        u32 n = 0;
        while (t[i] && !(t[i] == ' ' && (t[i + 1] == ' ' || t[i + 1] == 0))) {
            if (n < ConfigMenu::kCols)
                piece[n++] = t[i];
            ++i;
        }
        piece[n] = 0;
        c.text(left + static_cast<s32>(col * MenuPainter::kCell), y, piece, rgb, max_x);
    }
}

// A small computer, our menu bar mark (14 x 14).
void mark(const Canvas& c, s32 x, s32 y)
{
    c.fill(x + 1, y, 12, 10, kBlack);           // the case
    c.fill(x + 3, y + 2, 8, 6, kWhite);         // the screen
    c.fill(x + 4, y + 3, 6, 4, 0x3060C0);       // lit, in blue
    c.fill(x + 1, y + 10, 12, 3, kBlack);       // the base
    c.fill(x + 3, y + 11, 3, 1, kWhite);        // the drive slot
    c.fill(x, y + 13, 14, 1, kBlack);           // the foot
}

} // namespace

namespace ui {
u32 text_width(const char* t)
{
    u32 w = 0;
    for (; *t; ++t)
        w += glyph(*t).width + 1u;
    return w ? w - 1 : 0;
}
} // namespace ui

void MenuPainter::paint_splash(const ScreenSurface& out, const char* message, const char* hint)
{
    if (!out.base || (out.depth != 32 && out.depth != 16))
        return;
    const u32 k = scale_for(out.width, out.height);
    const Canvas c{out, k, out.width / k, out.height / k};
    const s32 W = static_cast<s32>(c.w), H = static_cast<s32>(c.h);
    for (s32 y = 0; y < H; ++y)
        for (s32 x = 0; x < W; ++x)
            c.dot(x, y, ((x ^ y) & 1) ? kDesk1 : kDesk2);
    // The mark at four times its size, on a white rounded tile.
    const Canvas big{out, k * 4, out.width / (k * 4), out.height / (k * 4)};
    const s32 bx = static_cast<s32>(big.w) / 2 - 7, by = static_cast<s32>(big.h) / 2 - 9;
    big.fill(bx - 3, by - 3, 20, 20, kBlack);
    big.fill(bx - 2, by - 2, 18, 18, kWhite);
    mark(big, bx, by);
    // The message on a white strip under it.
    const s32 ty = (by + 19) * 4 + 6;
    if (message && message[0]) {
        const s32 tw = static_cast<s32>(ui::text_width(message));
        c.fill(W / 2 - tw / 2 - 8, ty - 2, tw + 16, static_cast<s32>(ui::kLineHeight) + 4, kWhite);
        c.text(W / 2 - tw / 2, ty, message, kBlack);
    }
    if (hint && hint[0]) {
        const s32 hw = static_cast<s32>(ui::text_width(hint));
        c.fill(W / 2 - hw / 2 - 8, H - 30, hw + 16, static_cast<s32>(ui::kLineHeight) + 4, kWhite);
        c.text(W / 2 - hw / 2, H - 28, hint, kGrey);
    }
}

u32 MenuPainter::scale_for(u32 width, u32 height)
{
    // A window of 24 rows with a footer, and a margin around it: the
    // same scale for every page (a long file list is the tallest).
    const u32 need_w = kContentW + 2 * kPad + 2 + 40;
    const u32 need_h = kMenuBar + 1 + kTitleBar + 8 + 24 * ui::kLineHeight + 8 + 25 + 24;
    u32 k = 1;
    while (width / (k + 1) >= need_w && height / (k + 1) >= need_h)
        ++k;
    return k;
}

void MenuPainter::paint(const ConfigMenu::Screen& screen, const ScreenSurface& out, const char* hint)
{
    if (!out.base || (out.depth != 32 && out.depth != 16))
        return;
    const u32 k = scale_for(out.width, out.height);
    const Canvas c{out, k, out.width / k, out.height / k};
    const s32 W = static_cast<s32>(c.w), H = static_cast<s32>(c.h);

    // --- what goes where
    char title[ConfigMenu::kCols + 1] = "G3MacPi";
    s32 title_row = -1;
    for (u32 r = 0; r < ConfigMenu::kRows; ++r)
        if (screen.attr[r] == ConfigMenu::Attr::Title) {
            trimmed(screen.text[r], title, sizeof title);
            title_row = static_cast<s32>(r);
            break;
        }
    u32 rows[ConfigMenu::kRows], nrows = 0, status[ConfigMenu::kRows], nstatus = 0;
    for (u32 r = static_cast<u32>(title_row + 1); r < ConfigMenu::kRows; ++r) {
        if (screen.attr[r] == ConfigMenu::Attr::Status) {
            if (!empty(screen.text[r]))
                status[nstatus++] = r;
        } else {
            rows[nrows++] = r;
        }
    }
    while (nrows && empty(screen.text[rows[nrows - 1]]) && screen.attr[rows[nrows - 1]] != ConfigMenu::Attr::Selected)
        --nrows;
    // Runs of blank rows (the text menu pads to put its key help at the
    // bottom) become one.
    {
        u32 n = 0;
        for (u32 i = 0; i < nrows; ++i) {
            const bool blank = empty(screen.text[rows[i]]) && screen.attr[rows[i]] != ConfigMenu::Attr::Selected;
            if (blank && n && empty(screen.text[rows[n - 1]]))
                continue;
            rows[n++] = rows[i];
        }
        nrows = n;
    }

    // --- the desktop and the menu bar
    for (s32 y = static_cast<s32>(kMenuBar); y < H; ++y)
        for (s32 x = 0; x < W; ++x)
            c.dot(x, y, ((x ^ y) & 1) ? kDesk1 : kDesk2);
    c.fill(0, 0, W, kMenuBar - 1, kWhite);
    c.fill(0, kMenuBar - 1, W, 1, kBlack);
    static const u8 kCorner[5] = {5, 3, 2, 1, 1};  // the rounded top corners of the screen
    for (s32 y = 0; y < 5; ++y) {
        c.fill(0, y, kCorner[y], 1, kBlack);
        c.fill(W - kCorner[y], y, kCorner[y], 1, kBlack);
    }
    mark(c, 12, 3);
    s32 mx = c.text(36, 2, "G3MacPi", kBlack);
    c.text(mx + 16, 2, "Settings", kBlack);
    if (hint && hint[0]) {
        const s32 hw = static_cast<s32>(ui::text_width(hint));
        if (mx + 100 + hw < W)
            c.text(W - 14 - hw, 2, hint, kBlack);
    }

    // --- the window
    const s32 footer = nstatus ? 9 + static_cast<s32>(nstatus * ui::kLineHeight) : 0;
    const s32 ww = static_cast<s32>(kContentW + 2 * kPad + 2);
    const s32 wh = static_cast<s32>(1 + kTitleBar + 8 + nrows * ui::kLineHeight + 8 + 1) + footer;
    const s32 wx = (W - ww) / 2;
    s32 wy = static_cast<s32>(kMenuBar) + (H - static_cast<s32>(kMenuBar) - wh) / 2;
    if (wy < static_cast<s32>(kMenuBar) + 8)
        wy = static_cast<s32>(kMenuBar) + 8;
    c.fill(wx + 2, wy + 2, ww, wh, kBlack);  // the shadow
    c.fill(wx, wy, ww, wh, kBlack);          // the frame
    c.fill(wx + 1, wy + 1, ww - 2, wh - 2, kWhite);

    // Title bar: pinstripes, the close box, the title on a white gap.
    for (s32 y = wy + 4; y <= wy + 15; y += 2)
        c.fill(wx + 2, y, ww - 4, 1, kBlack);
    c.fill(wx + 1, wy + kTitleBar, ww - 2, 1, kBlack);
    c.fill(wx + 8, wy + 3, 15, 14, kWhite);    // clear the stripes around the close box
    c.fill(wx + 10, wy + 4, 11, 11, kBlack);   // the close box
    c.fill(wx + 11, wy + 5, 9, 9, kWhite);
    const s32 tw = static_cast<s32>(ui::text_width(title));
    const s32 tx = wx + (ww - tw) / 2;
    c.fill(tx - 7, wy + 2, tw + 14, 16, kWhite);
    c.text(tx, wy + 3, title, kBlack, wx + ww - 8);

    // Rows.
    const s32 left = wx + 1 + static_cast<s32>(kPad);
    const s32 right = left + static_cast<s32>(kContentW);
    s32 y = wy + 1 + static_cast<s32>(kTitleBar) + 8;
    for (u32 i = 0; i < nrows; ++i, y += ui::kLineHeight) {
        const char* t = screen.text[rows[i]];
        u32 rgb = kBlack;
        switch (screen.attr[rows[i]]) {
        case ConfigMenu::Attr::Selected:
            c.fill(left - 6, y, static_cast<s32>(kContentW) + 12, ui::kLineHeight, kBlack);
            rgb = kWhite;
            break;
        case ConfigMenu::Attr::Dim:
            rgb = kGrey;
            break;
        case ConfigMenu::Attr::Changed:
            rgb = kChanged;
            break;
        case ConfigMenu::Attr::Title:
            c.fill(left, y + ui::kLineHeight - 1, static_cast<s32>(kContentW), 1, kLine);
            break;
        default:
            break;
        }
        row_text(c, left - 2 * static_cast<s32>(kCell) + 4, y, t, rgb, right);
    }

    // Status footer.
    if (nstatus) {
        const s32 fy = wy + wh - 1 - footer;
        c.fill(wx + 1, fy, ww - 2, 1, kBlack);
        for (u32 i = 0; i < nstatus; ++i)
            row_text(c, left - 2 * static_cast<s32>(kCell) + 4, fy + 4 + static_cast<s32>(i * ui::kLineHeight),
                     screen.text[status[i]], kBlack, right);
    }
}

} // namespace g3
