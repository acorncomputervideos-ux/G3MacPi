// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/audio_bridge.cpp — see audio_bridge.h.

#include "platform/common/audio_bridge.h"

namespace g3 {

void AudioBridge::audio_out(const s16* frames, u32 count, u32 rate_hz)
{
    __atomic_store_n(&rate_, rate_hz, __ATOMIC_RELAXED);
    for (u32 i = 0; i < count; ++i) {
        if (q_.push(pack(frames[2 * i], frames[2 * i + 1])))
            ++st_.frames_in;
        else
            ++st_.frames_dropped;
    }
}

bool AudioBridge::next()
{
    u32 v;
    if (!q_.pop(v))
        return false;
    a_[0] = b_[0];
    a_[1] = b_[1];
    b_[0] = static_cast<s16>(v & 0xFFFF);
    b_[1] = static_cast<s16>(v >> 16);
    return true;
}

void AudioBridge::render(s16* out, u32 count, u32 out_rate)
{
    const u32 in_rate = __atomic_load_n(&rate_, __ATOMIC_RELAXED);
    // Keep the latency down: skip what is beyond the limit.
    const u32 max_q = in_rate / 1000 * kMaxLatencyMs;
    while (q_.size() > max_q) {
        u32 v;
        if (!q_.pop(v))
            break;
        ++st_.skipped;
    }
    if (!primed_) {
        // Start with two frames in hand (once there is something to play).
        if (q_.size() < 2) {
            for (u32 i = 0; i < 2 * count; ++i)
                out[i] = 0;
            st_.underruns += count;
            return;
        }
        next();
        next();
        phase_ = 0;
        primed_ = true;
    }
    const u64 step = (static_cast<u64>(in_rate) << 32) / (out_rate ? out_rate : 1);
    for (u32 i = 0; i < count; ++i) {
        const s64 f = static_cast<s64>(phase_ >> 16);  // Q16 fraction
        for (u32 c = 0; c < 2; ++c)
            out[2 * i + c] = static_cast<s16>(a_[c] + (((b_[c] - a_[c]) * f) >> 16));
        phase_ += step;
        while (phase_ >= (1ull << 32)) {
            if (!next()) {
                // Underrun: hold the last frame until more comes.
                a_[0] = b_[0];
                a_[1] = b_[1];
                phase_ = 0;
                ++st_.underruns;
                break;
            }
            phase_ -= 1ull << 32;
        }
        ++st_.frames_out;
    }
}

} // namespace g3
