// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/display_driver.cpp — see display_driver.h.

#include "core/firmware/openfirm/display_driver.h"

#include "core/common/cstring.h"

namespace g3::of {

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
