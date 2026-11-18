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

#ifndef G3_CORE_FIRMWARE_OPENFIRM_DISPLAY_DRIVER_H
#define G3_CORE_FIRMWARE_OPENFIRM_DISPLAY_DRIVER_H

#include "core/devices/video/framebuffer.h"
#include "core/firmware/openfirm/instances.h"

namespace g3::of {

class DisplayDriver final : public PackageDriver {
public:
    explicit DisplayDriver(Framebuffer& fb) : fb_(fb) {}
    bool open(u32, const char*) override { return true; }
    bool method(u32 slot, const char* name, forth::Forth& f) override;

    // Fills a rectangle with palette index `color` (clipped to the screen).
    void fill(u32 color, u32 x, u32 y, u32 w, u32 h);

private:
    Framebuffer& fb_;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_DISPLAY_DRIVER_H
