// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/audio_bridge.h — the Mac's sound from the emulation core
// to the core that plays it.
//
// The emulation core is the AudioSink (core/devices/macio/davbus.h): it
// pushes the codec's frames, at the codec's rate, into a lock-free queue.
// The I/O core calls render() for the frames its output device wants, at
// the device's rate (48 kHz for HDMI): the frames are resampled (linear
// interpolation), and the queue is kept short — when the Mac runs ahead of
// real time, frames beyond kMaxLatencyMs are skipped; when it falls behind,
// the output holds the last frame (a gap rather than a click).
//
// One producer core and one consumer core. Freestanding.

#ifndef G3_PLATFORM_COMMON_AUDIO_BRIDGE_H
#define G3_PLATFORM_COMMON_AUDIO_BRIDGE_H

#include "core/common/spsc_queue.h"
#include "core/common/types.h"
#include "core/devices/macio/davbus.h"

namespace g3 {

class AudioBridge final : public AudioSink {
public:
    static constexpr u32 kQueueFrames = 16384;  // 370 ms at 44.1 kHz
    static constexpr u32 kMaxLatencyMs = 120;

    struct Stats {
        u64 frames_in = 0, frames_dropped = 0;  // producer
        u64 frames_out = 0, underruns = 0, skipped = 0;  // consumer
    };

    // Producer (emulation core).
    void audio_out(const s16* frames, u32 count, u32 rate_hz) override;

    // Consumer (I/O core): count interleaved left/right frames at out_rate.
    void render(s16* out, u32 count, u32 out_rate);
    // Frames waiting (approximate).
    u32 queued() const { return q_.size(); }
    const Stats& stats() const { return st_; }

private:
    static u32 pack(s16 l, s16 r) { return static_cast<u16>(l) | (static_cast<u32>(static_cast<u16>(r)) << 16); }
    bool next();  // moves to the next input frame; false when none

    SpscQueue<u32, kQueueFrames> q_;
    u32 rate_ = 44100;  // written by the producer
    Stats st_;
    // Consumer state: the two input frames being interpolated, and the
    // position between them (Q32).
    s32 a_[2] = {0, 0}, b_[2] = {0, 0};
    u64 phase_ = 0;
    bool primed_ = false;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_AUDIO_BRIDGE_H
