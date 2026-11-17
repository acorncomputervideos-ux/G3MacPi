// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/cuda.cpp — see cuda.h.

#include "core/devices/macio/cuda.h"

namespace g3 {

namespace {
constexpr u64 kPsPerSecond = 1000000000000ull;
constexpr u8 kAcrSrOut = 0x10;  // VIA ACR: shift register output (host -> Cuda)
} // namespace

Cuda::Cuda(Via6522& via, Scheduler& scheduler) : via_(via), sched_(scheduler)
{
    via_.attach_peripheral(this);
    set_treq(false);
}

void Cuda::set_power_callbacks(EventCallback power_down, EventCallback reset, void* context)
{
    power_down_ = power_down;
    reset_ = reset;
    power_ctx_ = context;
}

void Cuda::set_rtc(u32 seconds)
{
    rtc_base_seconds_ = seconds;
    rtc_base_ps_ = sched_.now();
}

u32 Cuda::rtc() const
{
    return rtc_base_seconds_ + static_cast<u32>((sched_.now() - rtc_base_ps_) / kPsPerSecond);
}

void Cuda::set_treq(bool asserted)
{
    // Active low.
    via_.set_port_b_input(kTREQ, asserted ? 0 : kTREQ);
}

void Cuda::signal_byte()
{
    if (pending_ != Scheduler::kInvalidHandle)
        sched_.cancel(pending_);
    if (delay_ps_ == 0) {
        pending_ = Scheduler::kInvalidHandle;
        via_.raise_interrupt(Via6522::SR_INT);
        return;
    }
    pending_ = sched_.schedule_in(delay_ps_, sr_event, this);
}

void Cuda::sr_event(void* ctx, u64)
{
    auto* c = static_cast<Cuda*>(ctx);
    c->pending_ = Scheduler::kInvalidHandle;
    c->via_.raise_interrupt(Via6522::SR_INT);
}

void Cuda::load_next_out_byte()
{
    via_.set_shift_register(out_pos_ < out_len_ ? out_[out_pos_] : 0);
    ++out_pos_;
    if (out_pos_ >= out_len_)
        set_treq(false);  // TREQ is negated together with the last byte
}

void Cuda::via_port_b_changed(u8 value, u8 driven)
{
    // Only pins the host drives count; active low.
    const bool tip = (driven & kTIP) && !(value & kTIP);
    const bool tack = (driven & kTACK) && !(value & kTACK);
    const bool tip_changed = tip != tip_;
    const bool tack_changed = tack != tack_;
    tip_ = tip;
    tack_ = tack;
    if (!tip_changed && !tack_changed)
        return;
    const bool host_sending = (via_.acr() & kAcrSrOut) != 0;

    if (tip_changed && tip_) {
        // Start of a transfer.
        if (host_sending) {
            in_len_ = 0;
            in_[in_len_++] = via_.shift_register();
            state_ = State::Receiving;
            signal_byte();
        } else if (out_pending_) {
            state_ = State::Sending;
            out_pos_ = 0;
            load_next_out_byte();
            signal_byte();
        }
        return;
    }

    if (tip_changed && !tip_) {
        // End of a transfer.
        if (state_ == State::Receiving) {
            state_ = State::Idle;
            process_packet();  // may queue a reply and raise attention
            return;
        }
        if (state_ == State::Sending) {
            state_ = State::Idle;
            out_pending_ = false;
            set_treq(false);
            signal_byte();  // the host reads SR once more (read_done)
            return;
        }
        state_ = State::Idle;
        return;
    }

    // TACK toggled.
    if (tip_) {
        if (state_ == State::Receiving) {
            if (in_len_ < kMaxPacket)
                in_[in_len_++] = via_.shift_register();
            signal_byte();
        } else if (state_ == State::Sending) {
            load_next_out_byte();
            signal_byte();
        }
    } else if (state_ == State::Idle && !out_pending_) {
        // Idle synchronisation (Linux cuda_init_via): TREQ follows TACK.
        set_treq(tack_);
        signal_byte();
    }
}

// --- commands ----------------------------------------------------------------------

void Cuda::queue_reply(const u8* bytes, u32 len)
{
    out_len_ = len > kMaxPacket ? kMaxPacket : len;
    for (u32 i = 0; i < out_len_; ++i)
        out_[i] = bytes[i];
    out_pos_ = 0;
    out_pending_ = true;
    set_treq(true);   // attention
    signal_byte();
}

void Cuda::reply_cuda(u8 command, const u8* data, u32 len)
{
    u8 buf[kMaxPacket];
    buf[0] = kCudaPacket;
    buf[1] = 0;
    buf[2] = command;
    u32 n = 3;
    for (u32 i = 0; i < len && n < kMaxPacket; ++i)
        buf[n++] = data[i];
    queue_reply(buf, n);
}

void Cuda::reply_error(u8 code, u8 command)
{
    const u8 buf[4] = {kErrorPacket, code, kCudaPacket, command};
    queue_reply(buf, 4);
}

void Cuda::process_packet()
{
    if (in_len_ == 0)
        return;
    const u8 type = in_[0];
    if (type == kAdbPacket) {
        // No ADB devices yet: "no response" (flag bit 0x02).
        const u8 buf[3] = {kAdbPacket, 0x02, in_len_ > 1 ? in_[1] : static_cast<u8>(0)};
        queue_reply(buf, 3);
        return;
    }
    if (type != kCudaPacket || in_len_ < 2) {
        reply_error(2, in_len_ > 1 ? in_[1] : 0);
        return;
    }
    const u8 cmd = in_[1];
    const u8* args = in_ + 2;
    const u32 nargs = in_len_ - 2;

    switch (cmd) {
    case kAutopoll:
        if (nargs < 1) { reply_error(5, cmd); return; }
        autopoll_ = args[0] != 0;
        reply_cuda(cmd, nullptr, 0);
        return;
    case kGetTime: {
        const u32 t = rtc();
        const u8 d[4] = {static_cast<u8>(t >> 24), static_cast<u8>(t >> 16),
                         static_cast<u8>(t >> 8), static_cast<u8>(t)};
        reply_cuda(cmd, d, 4);
        return;
    }
    case kSetTime: {
        if (nargs < 4) { reply_error(5, cmd); return; }
        set_rtc((static_cast<u32>(args[0]) << 24) | (static_cast<u32>(args[1]) << 16) |
                (static_cast<u32>(args[2]) << 8) | args[3]);
        reply_cuda(cmd, nullptr, 0);
        return;
    }
    case kGetPram: {
        // Address is 16 bits; returns the byte at that address.
        // (Byte count of real Cuda replies: to verify.)
        if (nargs < 2) { reply_error(5, cmd); return; }
        const u32 addr = (static_cast<u32>(args[0]) << 8) | args[1];
        if (addr >= kPramSize) { reply_error(5, cmd); return; }
        reply_cuda(cmd, &pram_[addr], 1);
        return;
    }
    case kSetPram: {
        if (nargs < 3) { reply_error(5, cmd); return; }
        const u32 addr = (static_cast<u32>(args[0]) << 8) | args[1];
        for (u32 i = 2; i < nargs; ++i)
            if (addr + i - 2 < kPramSize)
                pram_[addr + i - 2] = args[i];
        reply_cuda(cmd, nullptr, 0);
        return;
    }
    case kSetAutoRate:
        if (nargs < 1 || args[0] == 0) { reply_error(5, cmd); return; }
        autopoll_rate_ms_ = args[0];
        reply_cuda(cmd, nullptr, 0);
        return;
    case kGetAutoRate:
        reply_cuda(cmd, &autopoll_rate_ms_, 1);
        return;
    case kSetDeviceList:
        if (nargs < 2) { reply_error(5, cmd); return; }
        device_list_ = static_cast<u16>((args[0] << 8) | args[1]);
        reply_cuda(cmd, nullptr, 0);
        return;
    case kGetDeviceList: {
        const u8 d[2] = {static_cast<u8>(device_list_ >> 8), static_cast<u8>(device_list_)};
        reply_cuda(cmd, d, 2);
        return;
    }
    case kPowerDown:
        reply_cuda(cmd, nullptr, 0);
        if (power_down_)
            power_down_(power_ctx_);
        return;
    case kResetSystem:
        reply_cuda(cmd, nullptr, 0);
        if (reset_)
            reset_(power_ctx_);
        return;
    default:
        reply_error(2, cmd);
        return;
    }
}

} // namespace g3
