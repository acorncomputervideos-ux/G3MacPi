// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/display_driver.h — the Open Firmware methods of the
// display node, on our framebuffer device.
//
// Methods (IEEE 1275 display device interface, from knowledge of the
// standard — to verify; color! order confirmed by Linux prom_init, see
// docs/firmware/display.md):
//   color! ( r g b index -- )          color@ ( index -- r g b )
//   set-colors ( adr index #indices -- ): adr holds r,g,b byte triples
//   fill-rectangle ( index x y w h -- )
//   dimensions ( -- width height )
// In 15- and 32-bit modes a colour index means its palette entry.
//
// write ( text ): a text terminal drawn with our 8x8 font (tools/font),
// each font row doubled: 8x16 cells. White (index 255) on black (0) with
// the power-on grey palette. \n = new line (column 0), \r = column 0,
// \b = back one column, \t = next multiple of 8, \f = clear screen;
// other control bytes are ignored, bytes >= 0x7F show as '?'. Lines wrap;
// the screen scrolls up at the bottom. Text is also passed to an echo
// sink (the boot log) if set.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_DISPLAY_DRIVER_H
#define G3_CORE_FIRMWARE_OPENFIRM_DISPLAY_DRIVER_H

#include "core/devices/video/framebuffer.h"
#include "core/firmware/openfirm/instances.h"

namespace g3::of {

class DisplayDriver final : public PackageDriver {
public:
    using Sink = void (*)(void* context, const char* text, u32 len);
    static constexpr u32 kCellW = 8, kCellH = 16;

    explicit DisplayDriver(Framebuffer& fb) : fb_(fb) {}
    bool open(u32, const char*) override { return true; }
    bool method(u32 slot, const char* name, forth::Forth& f) override;
    s32 write(u32 slot, const u8* buf, u32 len) override;
    s32 read(u32, u8*, u32) override { return 0; }  // no keyboard input here

    void set_echo(Sink s, void* ctx) { echo_ = s; echo_ctx_ = ctx; }
    void set_colors(u32 fg, u32 bg) { fg_ = fg & 255; bg_ = bg & 255; }
    void clear_screen();
    u32 cursor_column() const { return col_; }
    u32 cursor_row() const { return row_; }
    u32 columns() const { return fb_.width() / kCellW; }
    u32 rows() const { return fb_.height() / kCellH; }

    // Fills a rectangle with palette index `color` (clipped to the screen).
    void fill(u32 color, u32 x, u32 y, u32 w, u32 h);

private:
    void put_char(u8 c);
    void draw_glyph(u32 col, u32 row, u8 c);
    void new_line();
    void scroll();
    void pixel(u32 x, u32 y, u32 color);

    Framebuffer& fb_;
    Sink echo_ = nullptr;
    void* echo_ctx_ = nullptr;
    u32 col_ = 0, row_ = 0;
    u32 fg_ = 255, bg_ = 0;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_DISPLAY_DRIVER_H
