// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_audio_net_bridge.cpp — the queues between the
// emulation core and the I/O core: sound (resampled to the output rate,
// latency kept short, underruns hold) and Ethernet frames (in order, full
// queues drop), with each side on its own thread.

#include "platform/common/audio_bridge.h"
#include "platform/common/net_bridge.h"
#include "tests/framework/g3test.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using namespace g3;

TEST_CASE("platform/audio_bridge/resamples_44100_to_48000")
{
    AudioBridge b;
    // A ramp: left = i, right = -i.
    std::vector<s16> in(2 * 4410);
    for (u32 i = 0; i < 4410; ++i) {
        in[2 * i] = static_cast<s16>(i);
        in[2 * i + 1] = static_cast<s16>(-static_cast<s32>(i));
    }
    b.audio_out(in.data(), 4410, 44100);
    std::vector<s16> out(2 * 4800);
    b.render(out.data(), 4000, 48000);  // 4000 out = 3675 in
    // Output frame k sits at input position k * 44100 / 48000.
    for (u32 k = 0; k < 4000; ++k) {
        const double pos = k * 44100.0 / 48000.0;
        REQUIRE(std::abs(out[2 * k] - pos) <= 1.0);
        REQUIRE(std::abs(out[2 * k + 1] + pos) <= 1.0);
    }
    CHECK_EQ(b.stats().underruns, 0u);
    CHECK(b.queued() >= 4410 - 3677);
    CHECK(b.queued() <= 4410 - 3674);
}

TEST_CASE("platform/audio_bridge/latency_limit_and_underrun_hold")
{
    AudioBridge b;
    std::vector<s16> out(2 * 512);
    // Nothing yet: silence.
    b.render(out.data(), 512, 48000);
    for (s16 v : out)
        REQUIRE(v == 0);
    // 300 ms queued (the Mac ran ahead): rendering skips to the limit.
    std::vector<s16> in(2 * 13230, 1000);
    b.audio_out(in.data(), 13230, 44100);
    b.render(out.data(), 1, 48000);
    CHECK(b.queued() <= 44100 / 1000 * AudioBridge::kMaxLatencyMs);
    CHECK(b.stats().skipped >= 13230 - 44100 / 1000 * AudioBridge::kMaxLatencyMs - 2);
    // Run dry: the last value is held, not dropped to 0 (no click).
    b.render(out.data(), 512, 48000);
    std::vector<s16> big(2 * 8000);
    b.render(big.data(), 8000, 48000);
    CHECK(b.stats().underruns > 0);
    CHECK_EQ(big[2 * 7999], 1000);
    CHECK_EQ(big[2 * 7999 + 1], 1000);
}

TEST_CASE("platform/audio_bridge/two_threads")
{
    AudioBridge b;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::vector<s16> chunk(2 * 256);
        u32 n = 0;
        for (u32 k = 0; k < 400; ++k) {
            for (u32 i = 0; i < 256; ++i, ++n) {
                chunk[2 * i] = static_cast<s16>(n & 0x3FFF);
                chunk[2 * i + 1] = static_cast<s16>(n & 0x3FFF);
            }
            while (b.queued() > 4000)
                std::this_thread::yield();
            b.audio_out(chunk.data(), 256, 44100);
        }
        done = true;
    });
    std::vector<s16> out(2 * 441);
    u64 bad = 0;
    while (!done || b.queued() > 2) {
        b.render(out.data(), 441, 44100);  // same rate: frames pass through
        for (u32 i = 0; i < 441; ++i)
            bad += out[2 * i] != out[2 * i + 1];  // a torn frame would differ
    }
    producer.join();
    CHECK_EQ(bad, 0u);
    CHECK_EQ(b.stats().frames_in, 400u * 256u);
    CHECK_EQ(b.stats().frames_dropped, 0u);
}

TEST_CASE("platform/net_bridge/frames_both_ways")
{
    NetBridge n;
    u8 f[NetLink::kMaxFrame], got[NetLink::kMaxFrame];
    for (u32 i = 0; i < sizeof f; ++i)
        f[i] = static_cast<u8>(i * 7);
    // Out: in order, lengths kept.
    for (u32 k = 0; k < 5; ++k)
        n.net_send(f + k, 60 + k * 100);
    for (u32 k = 0; k < 5; ++k) {
        REQUIRE(n.take_outgoing(got) == 60 + k * 100);
        CHECK(std::memcmp(got, f + k, 60 + k * 100) == 0);
    }
    CHECK_EQ(n.take_outgoing(got), 0u);
    // In: full queue drops; too long is refused.
    for (u32 k = 0; k < NetBridge::kFrames; ++k)
        CHECK(n.put_incoming(f, 64));
    CHECK(!n.put_incoming(f, 64));
    CHECK_EQ(n.stats().in_dropped, 1u);
    CHECK_EQ(n.net_receive(got), 64u);
    CHECK(!n.put_incoming(f, NetLink::kMaxFrame + 1));
    // Out: full queue drops.
    for (u32 k = 0; k < NetBridge::kFrames + 3; ++k)
        n.net_send(f, 100);
    CHECK_EQ(n.stats().out_dropped, 3u);
}

TEST_CASE("platform/net_bridge/two_threads")
{
    NetBridge n;
    constexpr u32 kCount = 20000;
    std::thread io([&] {
        u8 buf[NetLink::kMaxFrame];
        u32 seen = 0;
        while (seen < kCount) {
            const u32 len = n.take_outgoing(buf);
            if (!len) {
                std::this_thread::yield();
                continue;
            }
            // Echo it back (the I/O core's side of a loopback).
            while (!n.put_incoming(buf, len))
                std::this_thread::yield();
            ++seen;
        }
    });
    u8 f[NetLink::kMaxFrame], got[NetLink::kMaxFrame];
    u32 sent = 0, back = 0, bad = 0;
    while (back < kCount) {
        if (sent < kCount && sent - back < NetBridge::kFrames) {
            const u32 len = 60 + sent % 1400;
            for (u32 i = 0; i < len; ++i)
                f[i] = static_cast<u8>(sent + i);
            n.net_send(f, len);
            ++sent;
        }
        const u32 len = n.net_receive(got);
        if (len) {
            bad += len != 60 + back % 1400;
            for (u32 i = 0; i < len; ++i)
                bad += got[i] != static_cast<u8>(back + i);
            ++back;
        }
    }
    io.join();
    CHECK_EQ(bad, 0u);
    CHECK_EQ(n.stats().out_dropped, 0u);
}
