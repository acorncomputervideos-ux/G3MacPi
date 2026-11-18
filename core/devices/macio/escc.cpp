// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/escc.cpp — see escc.h.

#include "core/devices/macio/escc.h"

namespace g3 {

namespace {
constexpr u8 kRr0TxEmpty = 0x04, kRr0TxEom = 0x40, kRr1AllSent = 0x01;
constexpr u8 kWr1TxIe = 0x02, kWr9Mie = 0x08;
} // namespace

void MacioEscc::reset_channel(u32 ch)
{
    Chan& c = chan_[ch & 1];
    for (u8& w : c.wr)
        w = 0;
    c.ptr = 0;
    c.tx_ip = false;
}

u8 MacioEscc::rr(u32 ch, u32 n) const
{
    const Chan& c = chan_[ch & 1];
    switch (n & 15) {
    case 0: case 4: return kRr0TxEmpty | kRr0TxEom;   // nothing received, nothing connected
    case 1: case 5: return kRr1AllSent;
    case 2: return chan_[A].wr[2];                     // the vector (shared)
    case 3:
        if (ch != A)
            return 0;
        return static_cast<u8>((chan_[B].tx_ip ? 0x02 : 0) | (chan_[A].tx_ip ? 0x10 : 0));
    case 8: return 0;                                  // receive data
    case 12: return c.wr[12];
    case 13: return c.wr[13];
    case 15: return c.wr[15];
    default: return 0;
    }
}

u8 MacioEscc::read_control(u32 ch)
{
    Chan& c = chan_[ch & 1];
    const u8 v = rr(ch, c.ptr);
    c.ptr = 0;
    return v;
}

void MacioEscc::write_control(u32 ch, u8 v)
{
    Chan& c = chan_[ch & 1];
    if (c.ptr == 0) {
        c.ptr = v & 7;
        switch ((v >> 3) & 7) {
        case 1: c.ptr += 8; break;                    // point high
        case 5: c.tx_ip = false; break;               // reset Tx interrupt pending
        default: break;                               // ext/status, error, IUS resets: nothing latched
        }
        c.wr[0] = v;
        update_irq();
        return;
    }
    const u32 n = c.ptr;
    c.ptr = 0;
    if (n == 9) {                                      // shared: resets and master interrupt enable
        if ((v & 0xC0) == 0xC0) {
            reset_channel(A);
            reset_channel(B);
        } else if (v & 0x80) {
            reset_channel(A);
        } else if (v & 0x40) {
            reset_channel(B);
        }
        wr9_ = v & 0x3F;
        chan_[A].wr[9] = chan_[B].wr[9] = wr9_;
    } else {
        c.wr[n] = v;
    }
    update_irq();
}

u8 MacioEscc::read_data(u32) { return 0; }

void MacioEscc::write_data(u32 ch, u8 v)
{
    Chan& c = chan_[ch & 1];
    if (tx_)
        tx_(tx_ctx_, ch & 1, v);
    c.tx_ip = (c.wr[1] & kWr1TxIe) != 0;              // sent at once: the buffer is empty again
    update_irq();
}

void MacioEscc::update_irq()
{
    for (u32 ch = 0; ch < 2; ++ch) {
        const bool now = (wr9_ & kWr9Mie) && chan_[ch].tx_ip;
        if (now != line_[ch]) {
            line_[ch] = now;
            if (irq_)
                irq_(irq_ctx_, ch, now);
        }
    }
}

bool MacioEscc::decode(u32 offset, u32& ch, bool& data)
{
    if (offset < 0x1000) {                             // legacy: B ctl 0, A ctl 2, B data 4, A data 6
        if (offset > 6 || (offset & 1))
            return false;
        ch = (offset & 2) ? A : B;
        data = (offset & 4) != 0;
        return true;
    }
    const u32 o = offset - 0x1000;                     // MacRISC: B at +0, A at +0x20; data +0x10
    if (o >= 0x40 || (o & 0xF))
        return false;
    ch = (o & 0x20) ? A : B;
    data = (o & 0x10) != 0;
    return true;
}

u32 MacioEscc::mmio_read(u32 offset, u32 size)
{
    u32 ch;
    bool data;
    u32 v = 0;
    if (decode(offset, ch, data))
        v = data ? read_data(ch) : read_control(ch);
    return size == 1 ? v : v << (8 * (size - 1));     // the byte lane of the addressed register
}

void MacioEscc::mmio_write(u32 offset, u32 size, u32 value)
{
    u32 ch;
    bool data;
    if (!decode(offset, ch, data))
        return;
    const u8 v = static_cast<u8>(value >> (8 * (size - 1)));
    if (data)
        write_data(ch, v);
    else
        write_control(ch, v);
}

} // namespace g3
