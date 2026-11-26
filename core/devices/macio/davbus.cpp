// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/davbus.cpp — see screamer.h.

#include "core/devices/macio/davbus.h"

#include "core/devices/le_register.h"

namespace g3 {

namespace {
// Attenuation steps 0..15 as Q15 gains, 1.5 dB apart (assumed; see the doc).
constexpr u32 kAtten[16] = {32768, 27571, 23198, 19519, 16423, 13818, 11627, 9783,
                            8231,  6925,  5827,  4903,  4125,  3471,  2920,  2457};
constexpr u32 kPicoseconds = 1000000000u;  // per millisecond

bool running(const DbdmaChannel* ch)
{
    return ch && (ch->status() & (dbdma::RUN | dbdma::ACTIVE)) == (dbdma::RUN | dbdma::ACTIVE);
}
} // namespace

MacioDavbus::MacioDavbus(Scheduler& scheduler, Codec codec) : sched_(scheduler), codec_type_(codec)
{
    out_.s = this;
    in_.s = this;
}

void MacioDavbus::attach_dma(DbdmaChannel* out, DbdmaChannel* in)
{
    out_ch_ = out;
    in_ch_ = in;
    if (out)
        out->attach_stream(&out_);
    if (in)
        in->attach_stream(&in_);
}

void MacioDavbus::gains(u32& left, u32& right) const
{
    if (codec_type_ == Codec::Burgundy) {
        // Some output enabled: the samples as they are (volume not applied).
        const bool on = (breg_[kBurgundyOutputs][0] & (0x80 | 0x06 | 0x18 | 0x60)) != 0;
        left = right = on ? 32768 : 0;
        return;
    }
    // Output C (the speaker) unless muted, else output A.
    const u32 r1 = codec_[1];
    u32 vol;
    if (!(r1 & (1u << 7)))
        vol = codec_[4];
    else if (!(r1 & (1u << 9)))
        vol = codec_[2];
    else {
        left = right = 0;
        return;
    }
    left = kAtten[(vol >> 6) & 0xF];
    right = kAtten[vol & 0xF];
}

u32 MacioDavbus::read_reg(u32 reg, bool consume)
{
    switch (reg) {
    case CONTROL: return control_;
    case CODEC_CTRL: return codec_ctrl_ & ~(1u << 24);  // the codec takes a word at once
    case CODEC_STAT:
        if (codec_type_ == Codec::Burgundy) {
            const u32 v = (static_cast<u32>(bdata_) << 4) | (bextend_ == 1 ? (1u << 23) : 0);
            if (consume && bextend_)
                bextend_ = bextend_ == 1 ? 2 : 0;
            return v;
        }
        return kCodecStatus;
    case CLIP: return 0;
    case BYTESWAP: return byteswap_;
    case FRAMES: return frame_count();
    default: return 0;
    }
}

u32 MacioDavbus::frame_count() const
{
    const u64 now = sched_.precise_now();
    const u64 dt = now > fc_ps_ ? now - fc_ps_ : 0;
    // dt * rate / 1e12 without overflow (dt < 2^64 / 2^16 for 8 years)
    return static_cast<u32>(fc_frames_ + (dt / 1000000u) * rate() / 1000000u);
}

void MacioDavbus::write_reg(u32 reg, u32 value)
{
    switch (reg) {
    case CONTROL: {
        if (((value ^ control_) >> 8) & 7) {  // a new rate: the clock goes on from here
            fc_frames_ = frame_count();
            fc_ps_ = sched_.precise_now();
        }
        // Error (11) and port change (12) clear when written with 1.
        const u32 sticky = control_ & ((1u << 11) | (1u << 12)) & ~value;
        control_ = (value & ~((1u << 11) | (1u << 12))) | sticky;
        return;
    }
    case CODEC_CTRL:
        codec_ctrl_ = value;
        if (codec_type_ == Codec::Burgundy) {
            const u32 op = (value >> 20) & 3, addr = (value >> 12) & 0xFF, byte = (value >> 8) & 3;
            if (op == 1) {
                bdata_ = breg_[addr][byte];
                bextend_ = 1;
            } else if (op == 2) {
                breg_[addr][byte] = static_cast<u8>(value);
            } else if (op == 3) {
                breg_[addr][0] = static_cast<u8>(value);
            }
            return;
        }
        codec_[(value >> 12) & 7] = value & 0xFFF;
        return;
    case BYTESWAP: byteswap_ = value & 1; return;
    default: return;
    }
}

u32 MacioDavbus::mmio_read(u32 offset, u32 size)
{
    return le_reg_read(read_reg(offset & 0xFF0, true), offset & 3, size);
}

void MacioDavbus::mmio_write(u32 offset, u32 size, u32 value)
{
    const u32 reg = offset & 0xFF0;
    write_reg(reg, le_reg_write(read_reg(reg, false), offset & 3, size, value));
}

u32 MacioDavbus::Out::dma_output(const u8* data, u32 len, bool)
{
    u32 room = kFifoBytes - s->fifo_count_;
    const u32 n = len < room ? len : room;
    for (u32 i = 0; i < n; ++i)
        s->fifo_[(s->fifo_head_ + s->fifo_count_ + i) % kFifoBytes] = data[i];
    s->fifo_count_ += n;
    s->start_ticks();
    return n;
}

u32 MacioDavbus::In::dma_input(u8* data, u32 len)
{
    const u32 n = len < s->in_avail_ ? len : s->in_avail_;
    for (u32 i = 0; i < n; ++i)
        data[i] = 0;
    s->in_avail_ -= n;
    s->start_ticks();
    return n;
}

void MacioDavbus::start_ticks()
{
    if (ticking_)
        return;
    ticking_ = true;
    sched_.schedule_in(static_cast<u64>(kTickFrames) * 1000u * kPicoseconds / rate(), tick_cb, this);
}

void MacioDavbus::tick(u64)
{
    ticking_ = false;
    // The frames of this interval, each taken from the FIFO, which the
    // output DMA refills as it empties.
    s16 buf[kTickFrames * 2];
    u32 gl, gr;
    gains(gl, gr);
    u32 frames = 0;
    while (frames < kTickFrames) {
        if (fifo_count_ < 4 && out_ch_) {
            ticking_ = true;  // a refill must not start another tick
            out_ch_->kick();
            ticking_ = false;
        }
        if (fifo_count_ < 4)
            break;  // underrun: the DMA has nothing more for now
        for (u32 c = 0; c < 2; ++c) {
            const u8 a = fifo_[(fifo_head_ + c * 2) % kFifoBytes];
            const u8 b = fifo_[(fifo_head_ + c * 2 + 1) % kFifoBytes];
            const s32 v = static_cast<s16>(byteswap_ ? (b << 8) | a : (a << 8) | b);
            buf[frames * 2 + c] = static_cast<s16>((v * static_cast<s32>(c ? gr : gl)) >> 15);
        }
        fifo_head_ = (fifo_head_ + 4) % kFifoBytes;
        fifo_count_ -= 4;
        ++frames;
    }
    if (frames) {
        frames_ += frames;
        if (sink_)
            sink_->audio_out(buf, frames, rate());
    }
    const bool in_on = running(in_ch_);
    if (in_on) {
        in_avail_ += kTickFrames * 4;
        if (in_avail_ > 2 * kTickFrames * 4)
            in_avail_ = 2 * kTickFrames * 4;  // the input FIFO holds two intervals
    }
    // Refill: the channels continue where they waited.
    if (out_ch_) {
        ticking_ = true;
        out_ch_->kick();
        ticking_ = false;
    }
    if (in_ch_) {
        ticking_ = true;
        in_ch_->kick();
        ticking_ = false;
    }
    if (!ticking_ && (fifo_count_ || running(out_ch_) || in_on))
        start_ticks();
}

} // namespace g3
