// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/escc.cpp — see escc.h.

#include "core/devices/macio/escc.h"

namespace g3 {

namespace {
constexpr u8 kRr0RxAvail = 0x01, kRr0TxEmpty = 0x04, kRr0Dcd = 0x08, kRr0Cts = 0x20, kRr0TxEom = 0x40;
constexpr u8 kRr1AllSent = 0x01, kRr1Overrun = 0x20;
constexpr u8 kWr1ExtIe = 0x01, kWr1TxIe = 0x02, kWr1RxMask = 0x18, kWr1RxFirst = 0x08, kWr1RxAll = 0x10;
constexpr u8 kWr3RxEnable = 0x01, kWr9Mie = 0x08, kWr15DcdIe = 0x08, kWr15CtsIe = 0x20;
} // namespace

void MacioEscc::reset_channel(u32 ch)
{
    Chan& c = chan_[ch & 1];
    for (u8& w : c.wr)
        w = 0;
    c.ptr = 0;
    c.tx_ip = c.ext_ip = c.rx_ip = false;
    c.rx_first = true;
    c.fifo_n = 0;
    c.overrun = false;
    report_lines(ch);  // DTR and RTS are off after a reset
}

void MacioEscc::report_lines(u32 ch)
{
    Chan& c = chan_[ch & 1];
    const bool d = (c.wr[5] & 0x80) != 0, r = (c.wr[5] & 0x02) != 0;
    if (d == c.lines_dtr && r == c.lines_rts)
        return;
    c.lines_dtr = d;
    c.lines_rts = r;
    if (lines_)
        lines_(lines_ctx_, ch & 1, d, r);
}

void MacioEscc::update_rx_ip(u32 ch)
{
    Chan& c = chan_[ch & 1];
    const u8 mode = c.wr[1] & kWr1RxMask;
    const bool special = c.overrun;
    if (mode == 0)
        c.rx_ip = false;
    else if (mode == kWr1RxAll)
        c.rx_ip = c.fifo_n > 0 || special;
    else if (mode == kWr1RxFirst)
        c.rx_ip = (c.fifo_n > 0 && c.rx_first) || special;
    else
        c.rx_ip = special;  // special conditions only
}

bool MacioEscc::rx_ready(u32 ch) const
{
    const Chan& c = chan_[ch & 1];
    return (c.wr[3] & kWr3RxEnable) && c.fifo_n < kRxFifo;
}

void MacioEscc::rx_push(u32 ch, u8 byte)
{
    Chan& c = chan_[ch & 1];
    if (!(c.wr[3] & kWr3RxEnable))
        return;  // the receiver is off: nothing is assembled
    if (c.fifo_n == kRxFifo)
        c.overrun = true;
    else
        c.fifo[c.fifo_n++] = byte;
    update_rx_ip(ch);
    update_irq();
    if (rx_dma_[ch & 1])
        rx_dma_[ch & 1]->kick();  // a waiting receive descriptor takes it
}

void MacioEscc::set_dcd(u32 ch, bool on)
{
    Chan& c = chan_[ch & 1];
    if (c.dcd == on)
        return;
    c.dcd = on;
    if ((c.wr[1] & kWr1ExtIe) && (c.wr[15] & kWr15DcdIe))
        c.ext_ip = true;
    update_irq();
}

void MacioEscc::set_cts(u32 ch, bool on)
{
    Chan& c = chan_[ch & 1];
    if (c.cts == on)
        return;
    c.cts = on;
    if ((c.wr[1] & kWr1ExtIe) && (c.wr[15] & kWr15CtsIe))
        c.ext_ip = true;
    update_irq();
}

u32 MacioEscc::baud(u32 ch) const
{
    const Chan& c = chan_[ch & 1];
    const u32 tc = c.wr[12] | static_cast<u32>(c.wr[13]) << 8;
    static const u32 kMode[4] = {1, 16, 32, 64};
    const u32 mode = kMode[c.wr[4] >> 6];
    if (!(c.wr[14] & 0x01))
        return 0;  // baud rate generator off
    return kClockHz / (2 * mode * (tc + 2));
}

u8 MacioEscc::rr(u32 ch, u32 n) const
{
    const Chan& c = chan_[ch & 1];
    switch (n & 15) {
    case 0: case 4:
        return static_cast<u8>(kRr0TxEmpty | kRr0TxEom | (c.fifo_n ? kRr0RxAvail : 0) | (c.dcd ? kRr0Dcd : 0) |
                               (c.cts ? kRr0Cts : 0));
    case 1: case 5: return static_cast<u8>(kRr1AllSent | (c.overrun ? kRr1Overrun : 0));
    case 2: return chan_[A].wr[2];                     // the vector (shared)
    case 3:
        if (ch != A)
            return 0;
        return static_cast<u8>((chan_[B].ext_ip ? 0x01 : 0) | (chan_[B].tx_ip ? 0x02 : 0) |
                               (chan_[B].rx_ip ? 0x04 : 0) | (chan_[A].ext_ip ? 0x08 : 0) |
                               (chan_[A].tx_ip ? 0x10 : 0) | (chan_[A].rx_ip ? 0x20 : 0));
    case 8: return c.fifo_n ? c.fifo[0] : 0;           // receive data (not taken: see read_data)
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
        case 2: c.ext_ip = false; break;              // reset ext/status interrupts
        case 4: c.rx_first = true; break;             // enable Rx interrupt on the next first character
        case 5: c.tx_ip = false; break;               // reset Tx interrupt pending
        case 6: c.overrun = false; break;             // error reset
        default: break;                               // IUS resets: nothing latched
        }
        c.wr[0] = v;
        update_rx_ip(ch);
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
        if (n == 1 && (v & kWr1RxMask) == kWr1RxFirst && (c.wr[1] & kWr1RxMask) != kWr1RxFirst)
            c.rx_first = true;  // the first character after the mode is selected interrupts
        c.wr[n] = v;
        if (n == 5)
            report_lines(ch);
        if (n == 3 && !(v & kWr3RxEnable))
            c.fifo_n = 0;  // receiver off: what it held is gone
    }
    update_rx_ip(A);
    update_rx_ip(B);
    update_irq();
}

bool MacioEscc::take(u32 ch, u8& byte)
{
    Chan& c = chan_[ch & 1];
    if (!c.fifo_n)
        return false;
    byte = c.fifo[0];
    for (u32 i = 1; i < c.fifo_n; ++i)
        c.fifo[i - 1] = c.fifo[i];
    --c.fifo_n;
    c.rx_first = false;  // "first character" mode: until RES_RxINT_FC
    update_rx_ip(ch);
    update_irq();
    return true;
}

u8 MacioEscc::read_data(u32 ch)
{
    u8 v = 0;
    take(ch, v);
    return v;
}

void MacioEscc::attach_dma(u32 ch, DbdmaChannel* tx, DbdmaChannel* rx)
{
    DmaPort& p = dma_[ch & 1];
    p.escc = this;
    p.ch = ch & 1;
    if (tx)
        tx->attach_stream(&p);
    if (rx)
        rx->attach_stream(&p);
    rx_dma_[ch & 1] = rx;
}

u32 MacioEscc::DmaPort::dma_output(const u8* data, u32 len, bool)
{
    for (u32 i = 0; i < len; ++i)
        escc->write_data(ch, data[i]);
    return len;
}

u32 MacioEscc::DmaPort::dma_input(u8* data, u32 len)
{
    u32 n = 0;
    while (n < len && escc->take(ch, data[n]))
        ++n;
    return n;
}

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
        const bool now = (wr9_ & kWr9Mie) && (chan_[ch].tx_ip || chan_[ch].rx_ip || chan_[ch].ext_ip);
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
