// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/screen_presenter.cpp — see screen_presenter.h.

#include "platform/common/screen_presenter.h"

namespace g3 {

namespace {

constexpr u32 kMaxWidth = 1920;
constexpr u32 kSnapshotTries = 1000;

u32 bytes_per_pixel(u32 depth) { return depth == 8 ? 1 : depth == 15 ? 2 : depth == 32 ? 4 : 0; }

// Row compare and copy, eight bytes at a time where possible.
bool same(const u8* a, const u8* b, u32 n)
{
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 x, y;
        __builtin_memcpy(&x, a + i, 8);
        __builtin_memcpy(&y, b + i, 8);
        if (x != y)
            return false;
    }
    for (; i < n; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

void copy(u8* d, const u8* s, u32 n)
{
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 x;
        __builtin_memcpy(&x, s + i, 8);
        __builtin_memcpy(d + i, &x, 8);
    }
    for (; i < n; ++i)
        d[i] = s[i];
}

void copy_frame(ScreenFrame& d, const ScreenFrame& s)
{
    d.on = s.on;
    d.width = s.width;
    d.height = s.height;
    d.depth = s.depth;
    d.pitch = s.pitch;
    d.vram = s.vram;
    d.vram_size = s.vram_size;
    for (u32 i = 0; i < 256; ++i)
        d.palette[i] = s.palette[i];
}

bool same_mode(const ScreenFrame& a, const ScreenFrame& b)
{
    return a.width == b.width && a.height == b.height && a.depth == b.depth && a.pitch == b.pitch &&
           a.vram == b.vram;
}

bool same_palette(const ScreenFrame& a, const ScreenFrame& b)
{
    for (u32 i = 0; i < 256; ++i)
        if (a.palette[i] != b.palette[i])
            return false;
    return true;
}

} // namespace

void ScreenPresenter::publish(const ScreenFrame& f)
{
    const u32 s = __atomic_load_n(&seq_, __ATOMIC_RELAXED);
    __atomic_store_n(&seq_, s + 1, __ATOMIC_RELAXED);  // odd: being written
    __atomic_thread_fence(__ATOMIC_RELEASE);
    copy_frame(pub_, f);
    __atomic_store_n(&seq_, s + 2, __ATOMIC_RELEASE);
}

bool ScreenPresenter::read_snapshot(ScreenFrame& f)
{
    for (u32 t = 0; t < kSnapshotTries; ++t) {
        const u32 s1 = __atomic_load_n(&seq_, __ATOMIC_ACQUIRE);
        if (s1 & 1)
            continue;
        copy_frame(f, pub_);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&seq_, __ATOMIC_RELAXED) == s1)
            return true;
    }
    return false;  // the publisher kept writing; try at the next frame
}

void ScreenPresenter::pause()
{
    __atomic_store_n(&paused_, 1u, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&busy_, __ATOMIC_SEQ_CST))
        ;
}

void ScreenPresenter::resume()
{
    __atomic_store_n(&redraw_all_, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&paused_, 0u, __ATOMIC_SEQ_CST);
}

void ScreenPresenter::clear(const ScreenSurface& out)
{
    const u32 row_bytes = out.width * (out.depth / 8);
    for (u32 y = 0; y < out.height; ++y) {
        u8* row = out.base + static_cast<size_t>(y) * out.pitch;
        if (out.depth == 32) {
            u32* p = reinterpret_cast<u32*>(row);
            for (u32 x = 0; x < out.width; ++x)
                p[x] = 0xFF000000u;
        } else {
            for (u32 i = 0; i < row_bytes; ++i)
                row[i] = 0;
        }
    }
}

u32 ScreenPresenter::render(const ScreenSurface& out)
{
    // Handshake with pause(): announce, then look (both sequentially consistent).
    __atomic_store_n(&busy_, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&paused_, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&busy_, 0u, __ATOMIC_SEQ_CST);
        return 0;
    }
    u32 drawn = 0;
    const bool redraw = __atomic_exchange_n(&redraw_all_, 0u, __ATOMIC_SEQ_CST) != 0;
    if (redraw)
        have_last_ = false;
    if (out.base && (out.depth == 16 || out.depth == 32) && read_snapshot(cur_)) {
        const u32 pb = bytes_per_pixel(cur_.depth);
        const u32 row_bytes = cur_.width * pb;
        const bool valid = cur_.on && cur_.vram && pb && cur_.width && cur_.height && cur_.width <= kMaxWidth &&
                           row_bytes <= cur_.pitch &&
                           static_cast<u64>(cur_.pitch) * (cur_.height - 1) + row_bytes <= cur_.vram_size &&
                           static_cast<u64>(row_bytes) * cur_.height <= shadow_size_;
        // The picture fills the host screen with the Mac's shape kept, as a
        // monitor shows any mode the Mac picks (Mac OS can switch modes at
        // its desktop): nearest pixel, so whole factors stay exact.
        u32 dw = 0, dh = 0;
        if (valid && out.width && out.height) {
            if (static_cast<u64>(out.width) * cur_.height <= static_cast<u64>(out.height) * cur_.width) {
                dw = out.width;
                dh = static_cast<u32>(static_cast<u64>(cur_.height) * out.width / cur_.width);
            } else {
                dh = out.height;
                dw = static_cast<u32>(static_cast<u64>(cur_.width) * out.height / cur_.height);
            }
            if (dw > kMaxOut)
                dw = dh = 0;
        }
        const u32 scale = dw && dh ? 1 : 0;  // 0: nothing can be shown
        const bool out_changed = out.base != last_out_base_ || out.width != last_out_w_ ||
                                 out.height != last_out_h_ || out.depth != last_out_depth_;
        if (!valid || scale == 0) {
            // Nothing to show (display off, or a mode too big for the host
            // screen): blank once.
            if (have_last_ || out_changed)
                clear(out);
            have_last_ = false;
        } else {
            const bool geometry = !have_last_ || !same_mode(cur_, last_) || out_changed;
            if (geometry)
                clear(out);  // the borders around a smaller picture
            const bool full = geometry || (cur_.depth == 8 && !same_palette(cur_, last_));
            const u32 x0 = (out.width - dw) / 2;
            const u32 y0 = (out.height - dh) / 2;
            if (geometry)
                for (u32 x = 0; x < dw; ++x)
                    xmap_[x] = static_cast<u16>(static_cast<u64>(x) * cur_.width / dw);
            for (u32 y = 0; y < cur_.height; ++y) {
                const u8* src = cur_.vram + static_cast<size_t>(y) * cur_.pitch;
                u8* shadow = shadow_ + static_cast<size_t>(y) * row_bytes;
                if (!full && same(src, shadow, row_bytes)) {
                    ++skipped_;
                    continue;
                }
                copy(shadow, src, row_bytes);
                // Convert from the copy (the Mac may be writing video memory).
                switch (cur_.depth) {
                case 8:
                    for (u32 x = 0; x < cur_.width; ++x)
                        line_[x] = cur_.palette[shadow[x]];
                    break;
                case 15:
                    for (u32 x = 0; x < cur_.width; ++x) {
                        const u32 p = (static_cast<u32>(shadow[2 * x]) << 8) | shadow[2 * x + 1];
                        const u32 r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
                        line_[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
                    }
                    break;
                default:  // 32
                    for (u32 x = 0; x < cur_.width; ++x)
                        line_[x] = (static_cast<u32>(shadow[4 * x + 1]) << 16) |
                                   (static_cast<u32>(shadow[4 * x + 2]) << 8) | shadow[4 * x + 3];
                    break;
                }
                // The host rows showing Mac row y: those r with r * h / dh == y.
                const u32 r0 = static_cast<u32>((static_cast<u64>(y) * dh + cur_.height - 1) / cur_.height);
                const u32 r1 = static_cast<u32>((static_cast<u64>(y + 1) * dh + cur_.height - 1) / cur_.height);
                for (u32 r = r0; r < r1 && r < dh; ++r) {
                    u8* row = out.base + static_cast<size_t>(y0 + r) * out.pitch;
                    if (out.depth == 32) {
                        u32* d = reinterpret_cast<u32*>(row) + x0;
                        for (u32 x = 0; x < dw; ++x)
                            d[x] = line_[xmap_[x]] | 0xFF000000u;
                    } else {
                        u16* d = reinterpret_cast<u16*>(row) + x0;
                        for (u32 x = 0; x < dw; ++x) {
                            const u32 p = line_[xmap_[x]];
                            d[x] = static_cast<u16>(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
                        }
                    }
                }
                ++drawn;
            }
            copy_frame(last_, cur_);
            have_last_ = true;
            rows_ += drawn;
            if (drawn)
                ++frames_;
        }
        last_out_base_ = out.base;
        last_out_w_ = out.width;
        last_out_h_ = out.height;
        last_out_depth_ = out.depth;
    }
    __atomic_store_n(&busy_, 0u, __ATOMIC_SEQ_CST);
    return drawn;
}

} // namespace g3
