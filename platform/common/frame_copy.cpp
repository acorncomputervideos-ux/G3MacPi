// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/frame_copy.cpp — see frame_copy.h.

#include "platform/common/frame_copy.h"

namespace g3 {

namespace {
bool same(const u8* a, const u8* b, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}
} // namespace

u32 copy_changed_rows(const u8* src, u32 src_pitch, u8* dst, u32 dst_pitch,
                      u32 row_bytes, u32 rows)
{
    u32 copied = 0;
    for (u32 y = 0; y < rows; ++y) {
        const u8* s = src + static_cast<u64>(y) * src_pitch;
        u8* d = dst + static_cast<u64>(y) * dst_pitch;
        if (same(s, d, row_bytes))
            continue;
        for (u32 i = 0; i < row_bytes; ++i)
            d[i] = s[i];
        ++copied;
    }
    return copied;
}

} // namespace g3
