// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/bmac.cpp — see bmac.h.

#include "core/devices/macio/bmac.h"

#include "core/devices/le_register.h"

namespace g3 {

namespace {
constexpr u32 kMinFrame = 60;  // without the CRC (IEEE 802.3)

// The Ethernet frame check sequence (IEEE 802.3 CRC-32, reflected).
u32 crc32(const u8* p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; ++i) {
        c ^= p[i];
        for (u32 k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
} // namespace

// --- PHY (IEEE 802.3 clause 22 registers) --------------------------------------------------

MacioBmac::Phy::Phy()
{
    reg[0] = 0x1000;  // control: auto-negotiation on
    reg[1] = 0x782D;  // status: 100/10 full/half, negotiation done, link up, extended registers
    reg[2] = 0x7810;  // ID: LXT970 (DingusPPC)
    reg[3] = 0x0000;
    reg[4] = 0x01E1;  // our abilities: 100/10 full/half, IEEE 802.3
    reg[5] = 0x41E1;  // the partner's, acknowledged
}

void MacioBmac::Phy::clock(bool driven, bool bit)
{
    // A frame: preamble of 1s, start 01, op (10 read, 01 write), PHY address
    // (5), register (5), turnaround, 16 data bits.
    switch (state) {
    case Idle:
        if (driven && !bit)
            state = Start;
        return;
    case Start:
        state = driven && bit ? Header : Idle;
        bits = n = 0;
        return;
    case Header:
        if (!driven) {
            state = Idle;
            return;
        }
        bits = (bits << 1) | bit;
        if (++n < 12)
            return;
        {
            const u32 op = bits >> 10, phy = (bits >> 5) & 31, r = bits & 31;
            if (op == 2) {
                const u32 data = phy == 0 ? reg[r] : 0xFFFF;
                // Turnaround (released, then 0), the data, then released.
                out = (1u << 17) | data;
                n = 18;
                state = ReadOut;
                mdi = true;
            } else if (op == 1) {
                bits = r | (phy << 5);
                out = 0;
                n = 0;
                state = WriteIn;
            } else {
                state = Idle;
            }
        }
        return;
    case ReadOut:
        // The bit for the slot just clocked has been read; present the next.
        --n;
        if (n == 0) {
            state = Idle;
            mdi = true;
        } else {
            mdi = (out >> (n - 1)) & 1;
        }
        return;
    case WriteIn:
        out = (out << 1) | (bit ? 1u : 0u);
        if (++n < 18)
            return;
        if ((bits >> 5) == 0) {
            const u32 r = bits & 31;
            u16 v = static_cast<u16>(out);
            if (r == 0)
                v &= 0x7FFF;  // reset completes at once
            if (r != 1 && r != 2 && r != 3)
                reg[r] = v;
        }
        state = Idle;
        return;
    }
}

// --- the cell -------------------------------------------------------------------------------

MacioBmac::MacioBmac(Scheduler& scheduler) : sched_(scheduler)
{
    tx_.m = this;
    rx_.m = this;
    regs_[INTDISABLE >> 4] = 0xFFFF;
}

void MacioBmac::attach_dma(DbdmaChannel* tx, DbdmaChannel* rx)
{
    tx_ch_ = tx;
    rx_ch_ = rx;
    if (tx)
        tx->attach_stream(&tx_);
    if (rx)
        rx->attach_stream(&rx_);
}

void MacioBmac::station_address(u8 out[6]) const
{
    const u16 w[3] = {regs_[MADD0 >> 4], regs_[MADD1 >> 4], regs_[MADD2 >> 4]};
    for (u32 i = 0; i < 3; ++i) {
        out[2 * i] = static_cast<u8>(w[i] >> 8);
        out[2 * i + 1] = static_cast<u8>(w[i]);
    }
}

void MacioBmac::update_irq()
{
    const bool want = (status_ & ~regs_[INTDISABLE >> 4]) != 0;
    if (want != line_) {
        line_ = want;
        if (irq_)
            irq_(irq_ctx_, want);
    }
}

void MacioBmac::raise(u16 bits)
{
    status_ |= bits;
    update_irq();
}

u16 MacioBmac::read_reg(u32 reg)
{
    switch (reg) {
    case CHIPID: return kChipId;
    case STATUS: {
        const u16 v = status_;
        status_ = 0;
        update_irq();
        return v;
    }
    case MIFCSR: return static_cast<u16>((regs_[reg >> 4] & 7) | (phy_.mdi ? 8 : 0));
    case SROMCSR: return static_cast<u16>(regs_[reg >> 4] & 7);
    case XCVRIF: return static_cast<u16>(regs_[reg >> 4] | LinkStatus);
    case TXRST: return 0;  // the reset is over at once
    default: return regs_[(reg >> 4) & 0x7F];
    }
}

void MacioBmac::write_reg(u32 reg, u16 value)
{
    switch (reg) {
    case CHIPID:
    case STATUS:
        return;
    case MIFCSR: {
        const u16 old = regs_[reg >> 4];
        regs_[reg >> 4] = value;
        if (!(old & 1) && (value & 1))
            phy_.clock((value & 4) != 0, (value & 2) != 0);
        return;
    }
    case INTDISABLE:
        regs_[reg >> 4] = value;
        update_irq();
        return;
    case TXRST:
        tx_len_ = 0;
        return;
    case RXCFG:
        regs_[reg >> 4] = value;
        if (value & RxMACEnable)
            start_poll();
        return;
    default:
        regs_[(reg >> 4) & 0x7F] = value;
        return;
    }
}

u32 MacioBmac::mmio_read(u32 offset, u32 size)
{
    if (offset & 0xC)
        return 0;
    return le_reg_read(read_reg(offset & 0xFF0), offset & 3, size);
}

void MacioBmac::mmio_write(u32 offset, u32 size, u32 value)
{
    if (offset & 0xC)
        return;
    const u32 reg = offset & 0xFF0;
    const u32 cur = reg == STATUS ? 0 : regs_[(reg >> 4) & 0x7F];
    write_reg(reg, static_cast<u16>(le_reg_write(cur, offset & 3, size, value)));
}

// --- transmit -----------------------------------------------------------------------------

u32 MacioBmac::Tx::dma_output(const u8* data, u32 len, bool last)
{
    for (u32 i = 0; i < len; ++i)
        if (m->tx_len_ < sizeof m->txbuf_)
            m->txbuf_[m->tx_len_++] = data[i];
    if (last) {
        if ((m->regs_[TXCFG >> 4] & TxMACEnable) && m->tx_len_ >= 14 && m->tx_len_ <= NetLink::kMaxFrame) {
            if (m->link_)
                m->link_->net_send(m->txbuf_, m->tx_len_);
            ++m->sent_;
            m->raise(FrameSent);
        } else {
            ++m->dropped_;
        }
        m->tx_len_ = 0;
    }
    return len;
}

// --- receive ------------------------------------------------------------------------------

bool MacioBmac::accept(const u8* f, u32 len) const
{
    if (len < 14)
        return false;
    const u16 cfg = regs_[RXCFG >> 4];
    u8 me[6];
    station_address(me);
    bool own_src = true, to_me = true, bcast = true;
    for (u32 i = 0; i < 6; ++i) {
        own_src = own_src && f[6 + i] == me[i];
        to_me = to_me && f[i] == me[i];
        bcast = bcast && f[i] == 0xFF;
    }
    if ((cfg & RxRejectOwn) && own_src)
        return false;
    if (cfg & RxPromisc)
        return true;
    if (to_me || bcast)
        return true;
    if (f[0] & 1) {  // multicast: taken whenever the hash filter wants any
        if (!(cfg & RxHashFilter))
            return false;
        for (u32 r = BHASH3; r <= BHASH0; r += 0x10)
            if (regs_[r >> 4])
                return true;
    }
    return false;
}

u32 MacioBmac::Rx::dma_input(u8* data, u32 len)
{
    const u32 left = m->rx_len_ - m->rx_pos_;
    const u32 n = len < left ? len : left;
    for (u32 i = 0; i < n; ++i)
        data[i] = m->rxbuf_[m->rx_pos_ + i];
    m->rx_pos_ += n;
    return n;
}

bool MacioBmac::Rx::dma_input_packet_end()
{
    if (m->rx_len_ == 0)
        return false;  // nothing yet: the descriptor waits
    m->rx_len_ = m->rx_pos_ = 0;  // done, or the rest did not fit
    ++m->received_;
    m->raise(FrameReceived);
    return true;
}

void MacioBmac::poll()
{
    if (!link_ || !rx_ch_)
        return;
    for (u32 k = 0; k < 16; ++k) {
        if (rx_len_ == 0) {
            const u32 n = link_->net_receive(rxbuf_);
            if (n == 0)
                return;
            if (n > NetLink::kMaxFrame || !(regs_[RXCFG >> 4] & RxMACEnable) || !accept(rxbuf_, n)) {
                ++dropped_;
                continue;
            }
            u32 len = n;
            while (len < kMinFrame)
                rxbuf_[len++] = 0;
            if (regs_[RXCFG >> 4] & RxCRCNoStrip) {
                const u32 c = crc32(rxbuf_, len);
                for (u32 i = 0; i < 4; ++i)
                    rxbuf_[len++] = static_cast<u8>(c >> (8 * i));
            }
            rx_len_ = len;
            rx_pos_ = 0;
        }
        rx_ch_->kick();
        if (rx_len_ != 0)
            return;  // no descriptor ready: try again next time
    }
}

void MacioBmac::start_poll()
{
    if (polling_)
        return;
    polling_ = true;
    sched_.schedule_in(kPollPs, poll_cb, this);
}

void MacioBmac::poll_tick()
{
    polling_ = false;
    poll();
    if (regs_[RXCFG >> 4] & RxMACEnable)
        start_poll();
}

} // namespace g3
