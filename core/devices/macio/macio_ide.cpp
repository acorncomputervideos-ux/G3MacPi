// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/macio_ide.cpp — see macio_ide.h.

#include "core/devices/macio/macio_ide.h"

#include "core/devices/le_register.h"

namespace g3 {

MacioIde::MacioIde() { ata_.set_dma_request(dma_request, this); }

void MacioIde::attach_dma(DbdmaChannel* channel)
{
    dma_ = channel;
    if (dma_)
        dma_->attach_stream(this);
}

void MacioIde::dma_request(void* ctx)
{
    auto* self = static_cast<MacioIde*>(ctx);
    if (self->dma_)
        self->dma_->kick();  // a channel already running may continue now
}

u32 MacioIde::mmio_read(u32 offset, u32 size)
{
    if (offset >= kTimingOffset && offset < kTimingOffset + 4)
        return le_reg_read(timing_, offset & 3, size);
    const u32 reg = offset >> 4;
    if (reg == 0) {
        // Data register: the stream's first byte is at the lowest address.
        u8 b[4] = {};
        const u32 n = size > 4 ? 4 : size;
        ata_.read_data(b, n);
        u32 v = 0;
        for (u32 i = 0; i < n; ++i)
            v = (v << 8) | b[i];
        return v;
    }
    if (size != 1)
        return 0xFFFFFFFFu >> (32 - 8 * size);  // byte registers only
    if (reg >= 1 && reg <= 7)
        return ata_.read_reg(reg);
    if (offset == kAltStatusOffset)
        return ata_.read_alt_status();
    return 0xFF;
}

void MacioIde::mmio_write(u32 offset, u32 size, u32 value)
{
    if (offset >= kTimingOffset && offset < kTimingOffset + 4) {
        timing_ = le_reg_write(timing_, offset & 3, size, value);
        return;
    }
    const u32 reg = offset >> 4;
    if (reg == 0) {
        u8 b[4] = {};
        const u32 n = size > 4 ? 4 : size;
        for (u32 i = 0; i < n; ++i)
            b[i] = static_cast<u8>(value >> (8 * (n - 1 - i)));
        ata_.write_data(b, n);
        return;
    }
    if (size != 1)
        return;
    if (reg >= 1 && reg <= 7)
        ata_.write_reg(reg, static_cast<u8>(value));
    else if (offset == kAltStatusOffset)
        ata_.write_device_control(static_cast<u8>(value));
}

u32 MacioIde::dma_output(const u8* data, u32 len, bool /*last*/)
{
    return ata_.dma_write(data, len);
}

u32 MacioIde::dma_input(u8* data, u32 len) { return ata_.dma_read(data, len); }

} // namespace g3
