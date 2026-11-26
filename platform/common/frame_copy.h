// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/frame_copy.h — show a picture drawn off-screen without
// flicker: copy to the visible frame buffer only the rows that differ.
//
// The settings menu repaints its whole screen for every key (desktop first,
// then the window over it). Drawn straight into the visible frame buffer,
// each repaint briefly shows the bare desktop where the window was: the
// flicker. Painting into a back buffer and copying the changed rows makes
// each row change once, from its old to its new contents; moving the
// selection changes only the two rows involved.
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_FRAME_COPY_H
#define G3_PLATFORM_COMMON_FRAME_COPY_H

#include "core/common/types.h"

namespace g3 {

// Copies each of `rows` rows of `row_bytes` bytes from src to dst where
// they differ. Returns the number of rows copied.
u32 copy_changed_rows(const u8* src, u32 src_pitch, u8* dst, u32 dst_pitch,
                      u32 row_bytes, u32 rows);

} // namespace g3

#endif // G3_PLATFORM_COMMON_FRAME_COPY_H
