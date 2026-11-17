// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/via6522.cpp — see via6522.h.

#include "core/devices/macio/via6522.h"

namespace g3 {

namespace {
using u128 = unsigned __int128;
constexpr u64 kPsPerSecond = 1000000000000ull;
} // namespace

Via6522::Via6522(Scheduler& scheduler) : sched_(scheduler) {}

void Via6522::set_irq_output(IrqCallback cb, void* context)
{
    irq_cb_ = cb;
    irq_ctx_ = context;
    if (irq_cb_)
        irq_cb_(irq_ctx_, irq_);
}

// --- time --------------------------------------------------------------------------

u64 Via6522::now_tick() const
{
    // tick = floor(now_ps * 4.7e6 / 6 / 1e12)
    return static_cast<u64>((static_cast<u128>(sched_.now()) * kClockNumerator) /
                            (static_cast<u128>(kClockDenominator) * kPsPerSecond));
}

u64 Via6522::tick_to_ps(u64 tick) const
{
    // Earliest time at which now_tick() >= tick (ceiling division).
    const u128 num = static_cast<u128>(tick) * kClockDenominator * kPsPerSecond;
    return static_cast<u64>((num + kClockNumerator - 1) / kClockNumerator);
}

u16 Via6522::counter(const Timer& t) const
{
    if (!t.running)
        return t.start_value;
    // The counter keeps decrementing (and wraps) after reaching zero.
    return static_cast<u16>(t.start_value - static_cast<u16>(now_tick() - t.start_tick));
}

void Via6522::start_timer(Timer& t, u16 value, bool is_t1)
{
    if (t.event != Scheduler::kInvalidHandle)
        sched_.cancel(t.event);
    t.running = true;
    t.start_value = value;
    t.start_tick = now_tick();
    // The interrupt flag is set when the counter passes from 0 to 0xFFFF,
    // i.e. value + 1 ticks after loading (simplified 6522 timing: N + 1).
    const u64 fire_tick = t.start_tick + static_cast<u64>(value) + 1;
    t.event = sched_.schedule_at(tick_to_ps(fire_tick), is_t1 ? t1_expired : t2_expired, this);
}

void Via6522::t1_expired(void* ctx, u64)
{
    auto* v = static_cast<Via6522*>(ctx);
    v->t1_.event = Scheduler::kInvalidHandle;
    v->ifr_ |= T1;
    if (v->acr_ & 0x40) {
        // Free-running: reload from the latch and continue.
        v->start_timer(v->t1_, v->t1_latch_, true);
    }
    v->update_irq();
}

void Via6522::t2_expired(void* ctx, u64)
{
    auto* v = static_cast<Via6522*>(ctx);
    v->t2_.event = Scheduler::kInvalidHandle;
    v->ifr_ |= T2;
    v->update_irq();
}

// --- interrupts ----------------------------------------------------------------------

void Via6522::update_irq()
{
    const bool now = (ifr_ & ier_ & 0x7F) != 0;
    if (now != irq_) {
        irq_ = now;
        if (irq_cb_)
            irq_cb_(irq_ctx_, irq_);
    }
}

void Via6522::raise_interrupt(u8 bits)
{
    ifr_ |= bits & 0x7F;
    update_irq();
}

void Via6522::set_port_b_input(u8 mask, u8 levels)
{
    pins_b_ = static_cast<u8>((pins_b_ & ~mask) | (levels & mask));
}

// --- registers -----------------------------------------------------------------------

u8 Via6522::read_reg(u32 r)
{
    switch (r) {
    case B:
        return static_cast<u8>((orb_ & ddrb_) | (pins_b_ & ~ddrb_));
    case A:
    case ANH:
        return ora_;  // port A: unused on the Power Mac VIA; plain storage
    case DIRB: return ddrb_;
    case DIRA: return ddra_;
    case T1CL:
        ifr_ &= ~T1;
        update_irq();
        return static_cast<u8>(counter(t1_));
    case T1CH: return static_cast<u8>(counter(t1_) >> 8);
    case T1LL: return static_cast<u8>(t1_latch_);
    case T1LH: return static_cast<u8>(t1_latch_ >> 8);
    case T2CL:
        ifr_ &= ~T2;
        update_irq();
        return static_cast<u8>(counter(t2_));
    case T2CH: return static_cast<u8>(counter(t2_) >> 8);
    case SR: {
        ifr_ &= ~SR_INT;
        update_irq();
        const u8 v = sr_;
        if (peripheral_)
            peripheral_->via_sr_read();
        return v;
    }
    case ACR: return acr_;
    case PCR: return pcr_;
    case IFR: return static_cast<u8>(ifr_ | ((ifr_ & ier_ & 0x7F) ? 0x80 : 0));
    case IER: return static_cast<u8>(ier_ | 0x80);
    }
    return 0;
}

void Via6522::write_reg(u32 r, u8 v)
{
    switch (r) {
    case B: {
        orb_ = v;
        if (peripheral_)
            peripheral_->via_port_b_changed(static_cast<u8>(orb_ & ddrb_), ddrb_);
        break;
    }
    case A:
    case ANH: ora_ = v; break;
    case DIRB:
        ddrb_ = v;
        if (peripheral_)
            peripheral_->via_port_b_changed(static_cast<u8>(orb_ & ddrb_), ddrb_);
        break;
    case DIRA: ddra_ = v; break;
    case T1CL:
    case T1LL: t1_latch_ = static_cast<u16>((t1_latch_ & 0xFF00) | v); break;
    case T1CH:
        t1_latch_ = static_cast<u16>((t1_latch_ & 0x00FF) | (v << 8));
        ifr_ &= ~T1;
        start_timer(t1_, t1_latch_, true);
        update_irq();
        break;
    case T1LH:
        t1_latch_ = static_cast<u16>((t1_latch_ & 0x00FF) | (v << 8));
        ifr_ &= ~T1;
        update_irq();
        break;
    case T2CL: t2_latch_lo_ = v; break;
    case T2CH:
        ifr_ &= ~T2;
        start_timer(t2_, static_cast<u16>(t2_latch_lo_ | (v << 8)), false);
        update_irq();
        break;
    case SR:
        sr_ = v;
        ifr_ &= ~SR_INT;
        update_irq();
        if (peripheral_)
            peripheral_->via_sr_written(v);
        break;
    case ACR:
        acr_ = v;
        break;
    case PCR: pcr_ = v; break;
    case IFR:
        ifr_ &= static_cast<u8>(~(v & 0x7F));  // write 1 to clear
        update_irq();
        break;
    case IER:
        if (v & 0x80)
            ier_ |= static_cast<u8>(v & 0x7F);
        else
            ier_ &= static_cast<u8>(~(v & 0x7F));
        update_irq();
        break;
    }
}

u32 Via6522::mmio_read(u32 offset, u32 size)
{
    const u8 v = read_reg((offset / kRegisterStride) & 0xF);
    // A wider access returns the register in its lowest-addressed byte;
    // the VIA only decodes byte accesses (not used by drivers).
    return size == 1 ? v : static_cast<u32>(v) << (8 * (size - 1));
}

void Via6522::mmio_write(u32 offset, u32 size, u32 value)
{
    const u8 v = static_cast<u8>(value >> (8 * (size - 1)));
    write_reg((offset / kRegisterStride) & 0xF, v);
}

} // namespace g3
