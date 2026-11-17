// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/nvram.cpp — see nvram.h.

#include "core/devices/macio/nvram.h"

namespace g3 {

MacioNvram::MacioNvram()
{
    for (u32 i = 0; i < kSize; ++i)
        data_[i] = 0;
}

void MacioNvram::load(const u8* bytes, u32 len)
{
    const u32 n = len < kSize ? len : kSize;
    for (u32 i = 0; i < n; ++i)
        data_[i] = bytes[i];
    dirty_ = false;
}

u32 MacioNvram::mmio_read(u32 offset, u32 size)
{
    u32 v = 0;
    for (u32 i = 0; i < size; ++i)
        v = (v << 8) | data_[((offset + i) / kStride) & (kSize - 1)];
    return v;
}

void MacioNvram::mmio_write(u32 offset, u32 size, u32 value)
{
    for (u32 i = 0; i < size; ++i) {
        const u8 b = static_cast<u8>(value >> (8 * (size - 1 - i)));
        u8& cell = data_[((offset + i) / kStride) & (kSize - 1)];
        if (cell != b) {
            cell = b;
            dirty_ = true;
        }
    }
}

} // namespace g3
