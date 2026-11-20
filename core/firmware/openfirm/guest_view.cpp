// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/guest_view.cpp — see guest_view.h.

#include "core/firmware/openfirm/guest_view.h"

namespace g3::of {

bool GuestView::read8(u32 a, u8& v)
{
    u32 p;
    return pa(a, false, p) && phys_.read8(p, v);
}

bool GuestView::write8(u32 a, u8 v)
{
    u32 p;
    return pa(a, true, p) && phys_.write8(p, v);
}

bool GuestView::read16(u32 a, u16& v)
{
    u32 p;
    if (one_page(a, 2))
        return pa(a, false, p) && phys_.read16(p, v);
    u8 b0, b1;
    if (!read8(a, b0) || !read8(a + 1, b1))
        return false;
    v = static_cast<u16>((b0 << 8) | b1);
    return true;
}

bool GuestView::read32(u32 a, u32& v)
{
    u32 p;
    if (one_page(a, 4))
        return pa(a, false, p) && phys_.read32(p, v);
    v = 0;
    for (u32 i = 0; i < 4; ++i) {
        u8 b;
        if (!read8(a + i, b))
            return false;
        v = (v << 8) | b;
    }
    return true;
}

bool GuestView::write16(u32 a, u16 v)
{
    u32 p;
    if (one_page(a, 2))
        return pa(a, true, p) && phys_.write16(p, v);
    return write8(a, static_cast<u8>(v >> 8)) && write8(a + 1, static_cast<u8>(v));
}

bool GuestView::write32(u32 a, u32 v)
{
    u32 p;
    if (one_page(a, 4))
        return pa(a, true, p) && phys_.write32(p, v);
    for (u32 i = 0; i < 4; ++i)
        if (!write8(a + i, static_cast<u8>(v >> (24 - 8 * i))))
            return false;
    return true;
}

} // namespace g3::of
