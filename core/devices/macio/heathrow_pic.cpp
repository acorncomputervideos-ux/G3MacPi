// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/heathrow_pic.cpp — see heathrow_pic.h.

#include "core/devices/macio/heathrow_pic.h"

#include "core/devices/le_register.h"

namespace g3 {

void HeathrowPic::set_output(OutputCallback cb, void* context)
{
    cb_ = cb;
    ctx_ = context;
    if (cb_)
        cb_(ctx_, output_);
}

void HeathrowPic::set_level_triggered(u64 mask)
{
    level_triggered_ = mask;
    events_ &= ~mask;  // level inputs never latch
    update_output();
}

void HeathrowPic::set_input(u32 irq, bool asserted)
{
    if (irq >= kNumInputs)
        return;
    const u64 bit = 1ull << irq;
    const bool was = (levels_ & bit) != 0;
    if (asserted) {
        levels_ |= bit;
        // Edge inputs latch on the 0 -> 1 transition...
        if (!was && !(level_triggered_ & bit))
            events_ |= bit;
    } else {
        levels_ &= ~bit;
        // ...and in 68K mode on 1 -> 0 as well.
        if (was && mode_68k() && !(level_triggered_ & bit))
            events_ |= bit;
    }
    update_output();
}

void HeathrowPic::update_output()
{
    // The mode bit enables no source.
    const bool now = ((events_ | (levels_ & level_triggered_)) & enable_ & ~kModeBit) != 0;
    if (now != output_) {
        output_ = now;
        if (cb_)
            cb_(ctx_, output_);
    }
}

int HeathrowPic::bank_for_offset(u32 offset)
{
    if (offset >= 0x20 && offset < 0x30)
        return 0;  // IRQs 0-31
    if (offset >= 0x10 && offset < 0x20)
        return 1;  // IRQs 32-63
    return -1;
}

u32 HeathrowPic::mmio_read(u32 offset, u32 size)
{
    const int bank = bank_for_offset(offset);
    if (bank < 0)
        return 0;  // unused part of the window reads as 0
    u32 reg = 0;
    switch (offset & 0xC) {
    case 0x0: reg = bank_bits(events_, bank); break;
    case 0x4:
        reg = bank_bits(enable_, bank);
        if (bank == 1)  // the mode bit, mirrored
            reg = (reg & 0x7FFFFFFFu) | (mode_68k() ? 0x80000000u : 0u);
        break;
    case 0x8: reg = 0; break;                       // ack is write-only
    case 0xC: reg = bank_bits(levels_, bank); break;
    }
    return le_reg_read(reg, offset & 3, size);
}

void HeathrowPic::mmio_write(u32 offset, u32 size, u32 value)
{
    const int bank = bank_for_offset(offset);
    if (bank < 0)
        return;
    const u32 shift = 32 * static_cast<u32>(bank);
    switch (offset & 0xC) {
    case 0x4: {  // enable
        const u32 cur = bank_bits(enable_, bank);
        u32 v = le_reg_write(cur, offset & 3, size, value);
        if (bank == 1)  // bit 31 here is the read-only mirror of the mode bit
            v = (v & 0x7FFFFFFFu) | (cur & 0x80000000u);
        enable_ = (enable_ & ~(0xFFFFFFFFull << shift)) | (static_cast<u64>(v) << shift);
        break;
    }
    case 0x8: {  // ack: clear latched edge events
        const u32 v = le_reg_write(0, offset & 3, size, value);
        if (bank == 0 && (v & 0x80000000u) && mode_68k()) {
            events_ = 0;  // 68K mode: bit 31 clears every event
            break;
        }
        events_ &= ~((static_cast<u64>(v & 0x7FFFFFFFu) << shift) & ~level_triggered_);
        break;
    }
    default:  // event and level are read-only
        return;
    }
    update_output();
}

} // namespace g3
