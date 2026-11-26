// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/screen_presenter.h — shows the Mac's screen from another
// core. The core running the Mac publishes a snapshot of the display (mode,
// where its video memory is, the palette); a second core renders from it
// onto the host's frame buffer, scaled to fill it with the Mac's aspect
// ratio kept (nearest pixel) and centred, and
// redraws only the rows of video memory that changed since its last frame
// (writes to the host frame buffer are slow; comparing rows is cheap).
//
// Threading: publish(), pause() and resume() on the Mac's core; render() on
// the drawing core. The snapshot is passed with a sequence lock, so
// publish() never waits. pause() returns once the drawing core is outside
// render() and won't enter it again until resume(): call it before the
// Mac's video memory goes away or before anything else draws on the host
// screen. Video memory itself is read while the Mac may be writing it, as
// a real display controller does; a row caught mid-update is redrawn on
// the next frame, because it is compared against what was drawn.
//
// Freestanding (GCC __atomic builtins).

#ifndef G3_PLATFORM_COMMON_SCREEN_PRESENTER_H
#define G3_PLATFORM_COMMON_SCREEN_PRESENTER_H

#include "core/common/types.h"

namespace g3 {

// What the Mac's display shows (published by the Mac's core).
struct ScreenFrame {
    bool on = false;            // the display is enabled
    u32 width = 0, height = 0;  // pixels
    u32 depth = 0;              // 8 (palette), 15 (xRRRRRGGGGGBBBBB, big-endian), 32 (xRGB, big-endian)
    u32 pitch = 0;              // bytes per row in video memory
    const u8* vram = nullptr;   // must stay valid until pause() returns
    u32 vram_size = 0;
    u32 palette[256] = {};      // 0x00RRGGBB, for depth 8
};

// Where to draw (the host frame buffer).
struct ScreenSurface {
    u8* base = nullptr;
    u32 width = 0, height = 0;  // pixels
    u32 pitch = 0;              // bytes per row
    u32 depth = 0;              // 16 (RGB565) or 32 (0xAARRGGBB, alpha set to 0xFF)
};

class ScreenPresenter {
public:
    // shadow: memory for a copy of the drawn video memory, at least as big
    // as the largest width * bytes-per-pixel * height to be shown.
    ScreenPresenter(u8* shadow, u32 shadow_size) : shadow_(shadow), shadow_size_(shadow_size) {}

    // Mac's core.
    void publish(const ScreenFrame& f);
    void pause();   // waits until render() is not running; render() then does nothing
    void resume();  // the next render() redraws everything
    // Drawing core: draws the latest frame if it changed. Returns the
    // number of rows of the Mac's screen redrawn (0: nothing to do).
    u32 render(const ScreenSurface& out);

    // Statistics (drawing core).
    u32 frames_rendered() const { return frames_; }
    u64 rows_redrawn() const { return rows_; }
    u64 rows_skipped() const { return skipped_; }

private:
    bool read_snapshot(ScreenFrame& f);
    void clear(const ScreenSurface& out, u32 x0, u32 y0, u32 w, u32 h);

    // The published snapshot (sequence lock: odd while being written).
    u32 seq_ = 0;
    ScreenFrame pub_;

    // pause() handshake.
    u32 paused_ = 0;
    u32 busy_ = 0;
    u32 redraw_all_ = 1;  // set by resume(), read by render()

    // Drawing core's state.
    ScreenFrame cur_;
    ScreenFrame last_;
    bool have_last_ = false;
    u32 last_out_w_ = 0, last_out_h_ = 0, last_out_depth_ = 0;
    u8* last_out_base_ = nullptr;
    u8* shadow_;
    u32 shadow_size_;
    u32 line_[1920] = {};  // one converted row
    static constexpr u32 kMaxOut = 4096;  // host screen width
    u16 xmap_[kMaxOut] = {};  // host column -> Mac column
    u32 frames_ = 0;
    u64 rows_ = 0, skipped_ = 0;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_SCREEN_PRESENTER_H
