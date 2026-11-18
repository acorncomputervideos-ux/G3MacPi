// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/display_driver.cpp — see display_driver.h.

#include "core/firmware/openfirm/display_driver.h"

#include "core/common/cstring.h"

namespace g3::of {

namespace {
const u8 kFont[95][8] = {
#include "core/firmware/openfirm/font8x8.inc"
};
} // namespace

void DisplayDriver::pixel(u32 x, u32 y, u32 color)
{
    u8* p = fb_.vram() + static_cast<size_t>(y) * fb_.pitch();
    switch (fb_.depth()) {
    case 8:
        p[x] = static_cast<u8>(color);
        break;
    case 15: {
        const u32 rgb = fb_.palette(color);
        const u32 v = (((rgb >> 19) & 31) << 10) | (((rgb >> 11) & 31) << 5) | ((rgb >> 3) & 31);
        p[2 * x] = static_cast<u8>(v >> 8);
        p[2 * x + 1] = static_cast<u8>(v);
        break;
    }
    case 32: {
        const u32 rgb = fb_.palette(color);
        p[4 * x] = 0;
        p[4 * x + 1] = static_cast<u8>(rgb >> 16);
        p[4 * x + 2] = static_cast<u8>(rgb >> 8);
        p[4 * x + 3] = static_cast<u8>(rgb);
        break;
    }
    default:
        break;
    }
}

void DisplayDriver::draw_glyph(u32 col, u32 row, u8 c)
{
    const u8* g = kFont[(c >= 0x20 && c < 0x7F) ? c - 0x20 : '?' - 0x20];
    const u32 x0 = col * kCellW, y0 = row * kCellH;
    for (u32 y = 0; y < kCellH; ++y) {
        const u8 bits = g[y / 2];  // each font row twice
        for (u32 x = 0; x < kCellW; ++x)
            pixel(x0 + x, y0 + y, (bits & (0x80u >> x)) ? fg_ : bg_);
    }
}

void DisplayDriver::clear_screen()
{
    fill(bg_, 0, 0, fb_.width(), fb_.height());
    col_ = row_ = 0;
}

void DisplayDriver::scroll()
{
    const u32 line = kCellH * fb_.pitch();
    const u32 used = rows() * line;
    u8* v = fb_.vram();
    for (u32 i = 0; i + line < used; ++i)
        v[i] = v[i + line];
    fill(bg_, 0, (rows() - 1) * kCellH, fb_.width(), kCellH);
}

void DisplayDriver::new_line()
{
    col_ = 0;
    if (row_ + 1 < rows()) {
        ++row_;
    } else {
        scroll();
    }
}

void DisplayDriver::put_char(u8 c)
{
    switch (c) {
    case '\n': new_line(); return;
    case '\r': col_ = 0; return;
    case '\b': if (col_) --col_; return;
    case '\t':
        col_ = (col_ + 8) & ~7u;
        if (col_ >= columns())
            new_line();
        return;
    case '\f': clear_screen(); return;
    default: break;
    }
    if (c < 0x20)
        return;  // other control bytes
    if (col_ >= columns())
        new_line();
    draw_glyph(col_, row_, c >= 0x7F ? '?' : c);
    ++col_;
}

s32 DisplayDriver::write(u32, const u8* buf, u32 len)
{
    if (columns() && rows())
        for (u32 i = 0; i < len; ++i)
            put_char(buf[i]);
    if (echo_)
        echo_(echo_ctx_, reinterpret_cast<const char*>(buf), len);
    return static_cast<s32>(len);
}

void DisplayDriver::fill(u32 color, u32 x, u32 y, u32 w, u32 h)
{
    const u32 W = fb_.width(), H = fb_.height();
    if (x >= W || y >= H)
        return;
    if (w > W - x)
        w = W - x;
    if (h > H - y)
        h = H - y;
    const u32 rgb = fb_.palette(color & 255);
    u8* vram = fb_.vram();
    for (u32 row = y; row < y + h; ++row) {
        u8* p = vram + static_cast<size_t>(row) * fb_.pitch();
        for (u32 col = x; col < x + w; ++col) {
            switch (fb_.depth()) {
            case 8:
                p[col] = static_cast<u8>(color);
                break;
            case 15: {
                const u32 v = (((rgb >> 19) & 31) << 10) | (((rgb >> 11) & 31) << 5) | ((rgb >> 3) & 31);
                p[2 * col] = static_cast<u8>(v >> 8);
                p[2 * col + 1] = static_cast<u8>(v);
                break;
            }
            case 32:
                p[4 * col] = 0;
                p[4 * col + 1] = static_cast<u8>(rgb >> 16);
                p[4 * col + 2] = static_cast<u8>(rgb >> 8);
                p[4 * col + 3] = static_cast<u8>(rgb);
                break;
            default:
                return;
            }
        }
    }
}

bool DisplayDriver::method(u32, const char* name, forth::Forth& f)
{
    using forth::Cell;
    if (cstr::equal(name, "color!")) {  // ( r g b index -- )
        const Cell index = f.pop(), b = f.pop(), g = f.pop(), r = f.pop();
        if (!f.failed())
            fb_.set_palette(index, ((r & 255) << 16) | ((g & 255) << 8) | (b & 255));
        return true;
    }
    if (cstr::equal(name, "color@")) {  // ( index -- r g b )
        const Cell index = f.pop();
        if (f.failed())
            return true;
        const u32 rgb = fb_.palette(index);
        f.push((rgb >> 16) & 255);
        f.push((rgb >> 8) & 255);
        f.push(rgb & 255);
        return true;
    }
    if (cstr::equal(name, "set-colors")) {  // ( adr index #indices -- )
        const Cell n = f.pop(), index = f.pop(), adr = f.pop();
        if (f.failed())
            return true;
        for (u32 i = 0; i < n && i < 256; ++i) {
            u8 rgb[3];
            for (u32 k = 0; k < 3; ++k)
                if (!f.memory().read8(adr + 3 * i + k, rgb[k])) {
                    f.fail(forth::Forth::kInvalidAddress, "invalid address");
                    return true;
                }
            fb_.set_palette(index + i, (static_cast<u32>(rgb[0]) << 16) | (rgb[1] << 8) | rgb[2]);
        }
        return true;
    }
    if (cstr::equal(name, "fill-rectangle")) {  // ( index x y w h -- )
        const Cell h = f.pop(), w = f.pop(), y = f.pop(), x = f.pop(), index = f.pop();
        if (!f.failed())
            fill(index, x, y, w, h);
        return true;
    }
    if (cstr::equal(name, "dimensions")) {  // ( -- width height )
        f.push(fb_.width());
        f.push(fb_.height());
        return true;
    }
    return false;
}

} // namespace g3::of
