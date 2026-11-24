// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/devices/test_davbus.cpp — the sound cell (DAV bus) with its Screamer and Burgundy codecs, driven
// the way Linux's snd-powermac does: codec words through codec control, a
// ring of OUTPUT_MORE descriptors that branches back, samples taken at the
// codec's rate in guest time.

#include "core/common/endian.h"
#include "core/devices/macio/davbus.h"
#include "core/sched/scheduler.h"
#include "tests/framework/g3test.h"

#include <cstdio>
#include <vector>

using namespace g3;
using namespace g3::dbdma;

namespace {

constexpr u32 kDma = 0x80808000u, kSnd = 0x80814000u;
constexpr u32 kOut = 0x800 / 0x80, kIn = 0x900 / 0x80;
constexpr u64 kMs = 1000000000ull;

struct Sink final : AudioSink {
    std::vector<s16> samples;
    u32 rate = 0, calls = 0;
    void audio_out(const s16* f, u32 n, u32 r) override
    {
        samples.insert(samples.end(), f, f + 2 * n);
        rate = r;
        ++calls;
    }
    size_t frames() const { return samples.size() / 2; }
};

constexpr u16 cmd(u16 c, u16 i = 0, u16 b = 0) { return static_cast<u16>((c << 12) | (i << 4) | (b << 2)); }

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x40000);
    PhysicalBus bus;
    Scheduler sched;
    DbdmaController dma{bus};
    MacioDavbus snd{sched};
    Sink sink;
    int out_irqs = 0;
    Rig()
    {
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        bus.map_mmio(kDma, DbdmaController::kWindowSize, &dma);
        bus.map_mmio(kSnd, MacioDavbus::kWindowSize, &snd);
        snd.attach_dma(&dma.channel(kOut), &dma.channel(kIn));
        snd.set_sink(&sink);
        dma.channel(kOut).set_irq([](void* c) { ++*static_cast<int*>(c); }, &out_irqs);
    }
    void out_le32(u32 a, u32 v) { bus.write32(a, byteswap32(v)); }
    u32 in_le32(u32 a)
    {
        u32 v = 0;
        bus.read32(a, v);
        return byteswap32(v);
    }
    void codec(u32 reg, u32 val) { out_le32(kSnd + 0x10, (reg << 12) | val); }
    void desc(u32 at, u16 c, u16 req, u32 addr, u32 dep = 0)
    {
        store_le16(&ram[at + 0], req);
        store_le16(&ram[at + 2], c);
        store_le32(&ram[at + 4], addr);
        store_le32(&ram[at + 8], dep);
        store_le32(&ram[at + 12], 0);
    }
    // snd-powermac's playback ring: periods of OUTPUT_MORE + INTR_ALWAYS,
    // then NOP + BR_ALWAYS back to the start.
    void ring(u32 channel, u32 cmds, u32 buf, u32 periods, u32 period_bytes)
    {
        for (u32 i = 0; i < periods; ++i)
            desc(cmds + 16 * i, cmd(channel == kOut ? OUTPUT_MORE : INPUT_MORE, 3), static_cast<u16>(period_bytes),
                 buf + i * period_bytes);
        desc(cmds + 16 * periods, cmd(NOP, 0, 3), 0, 0, cmds);
        const u32 regs = kDma + channel * DbdmaController::kChannelStride;
        out_le32(regs + 0x0C, cmds);
        out_le32(regs + 0x00, ((RUN | WAKE) << 16) | RUN | WAKE);
    }
};

// Frame f, channel c of the test signal.
s16 sample(u32 f, u32 c) { return static_cast<s16>(f * 37 + c * 1000 - 20000); }

void fill(Rig& r, u32 at, u32 frames, bool little)
{
    for (u32 f = 0; f < frames; ++f)
        for (u32 c = 0; c < 2; ++c) {
            const u16 v = static_cast<u16>(sample(f, c));
            u8* p = &r.ram[at + f * 4 + c * 2];
            p[little ? 1 : 0] = static_cast<u8>(v >> 8);
            p[little ? 0 : 1] = static_cast<u8>(v);
        }
}

} // namespace

TEST_CASE("devices/davbus/screamer_registers_as_linux_reads_them")
{
    Rig r;
    const u32 stat = r.in_le32(kSnd + 0x20);
    CHECK(stat & (1u << 22));                 // MASK_VALID: snd_pmac_screamer_wait goes on
    CHECK_EQ((stat >> 8) & 0xF, 1u);          // manufacturer: Crystal
    CHECK_EQ((stat >> 12) & 0xF, 3u);         // revision 3
    r.codec(1, 0x280);                        // mute A and C
    CHECK_EQ(r.in_le32(kSnd + 0x10) & (1u << 24), 0u);  // MASK_NEWECMD clear: written
    CHECK_EQ(r.snd.codec_reg(1), 0x280u);
    r.codec(4, 0x3CF);
    CHECK_EQ(r.snd.codec_reg(4), 0x3CFu);
    CHECK_EQ(r.snd.codec_reg(1), 0x280u);     // other registers untouched
    // Control: the rate in bits 8-10 (snd_pmac_pcm_set_format).
    r.out_le32(kSnd + 0x00, (0x11 | (3u << 13)) | (2u << 8));
    CHECK_EQ(r.snd.rate(), 22050u);
    CHECK_EQ(r.in_le32(kSnd + 0x00), (0x11u | (3u << 13)) | (2u << 8));
    r.out_le32(kSnd + 0x40, 1);
    CHECK_EQ(r.in_le32(kSnd + 0x40), 1u);
    // Byte access to a register's low byte (byte lanes, little-endian).
    u8 b = 0;
    r.bus.read8(kSnd + 0x01, b);
    CHECK_EQ(b, 0x62u);  // rate 2 and the interrupt enables (bits 13, 14)
}

TEST_CASE("devices/davbus/screamer_plays_at_the_codec_rate")
{
    Rig r;
    r.codec(1, 0);  // nothing muted, attenuation 0: samples pass unchanged
    r.codec(4, 0);
    constexpr u32 kPeriods = 4, kPeriodBytes = 2048, kFrames = kPeriods * kPeriodBytes / 4;
    fill(r, 0x10000, kFrames, false);
    r.ring(kOut, 0x1000, 0x10000, kPeriods, kPeriodBytes);
    // 44.1 kHz: in 100 ms, 4410 frames (to one tick of 256).
    r.sched.advance_to(100 * kMs);
    CHECK(r.sink.frames() + MacioDavbus::kTickFrames >= 4410);
    CHECK(r.sink.frames() <= 4410);
    CHECK_EQ(r.sink.rate, 44100u);
    for (size_t f = 0; f < r.sink.frames(); ++f) {
        REQUIRE(r.sink.samples[2 * f] == sample(static_cast<u32>(f % kFrames), 0));
        REQUIRE(r.sink.samples[2 * f + 1] == sample(static_cast<u32>(f % kFrames), 1));
    }
    // One interrupt per period consumed: 4410 frames = 8.6 periods.
    CHECK(r.out_irqs >= 8);
    CHECK(r.out_irqs <= 11);
    // It keeps going round the ring.
    r.sched.advance_to(1000 * kMs);
    CHECK(r.sink.frames() + MacioDavbus::kTickFrames >= 44100);
    CHECK(r.sink.frames() <= 44100);
    CHECK_EQ(r.snd.frames_played(), r.sink.frames());
}

TEST_CASE("devices/davbus/screamer_volume_mute_byteswap_and_rate")
{
    Rig r;
    r.codec(1, 1u << 7);                 // speaker muted: output A (reg 2) plays
    r.codec(2, (15u << 6) | 0);          // left fully attenuated, right not
    r.out_le32(kSnd + 0x40, 1);          // little-endian samples
    r.out_le32(kSnd + 0x00, 5u << 8);    // 11025 Hz
    fill(r, 0x10000, 2048, true);
    r.ring(kOut, 0x1000, 0x10000, 4, 2048);
    r.sched.advance_to(100 * kMs);
    CHECK(r.sink.frames() + MacioDavbus::kTickFrames >= 1102);
    CHECK(r.sink.frames() <= 1103);
    CHECK_EQ(r.sink.rate, 11025u);
    REQUIRE(r.sink.frames() > 10);
    for (u32 f = 0; f < 10; ++f) {
        CHECK_EQ(r.sink.samples[2 * f + 1], sample(f, 1));
        CHECK_EQ(r.sink.samples[2 * f], static_cast<s16>((sample(f, 0) * 2457) >> 15));
    }
    // Both outputs muted: silence.
    r.codec(1, (1u << 7) | (1u << 9));
    const size_t before = r.sink.frames();
    r.sched.advance_to(200 * kMs);
    REQUIRE(r.sink.frames() > before);
    for (size_t i = 2 * before; i < r.sink.samples.size(); ++i)
        REQUIRE(r.sink.samples[i] == 0);
}

TEST_CASE("devices/davbus/screamer_stops_with_the_dma_and_input_is_paced_silence")
{
    Rig r;
    // Output: one period then STOP: exactly those frames come out, then
    // nothing more is scheduled.
    fill(r, 0x10000, 512, false);
    r.desc(0x1000, cmd(OUTPUT_MORE, 3), 2048, 0x10000);
    r.desc(0x1010, cmd(STOP), 0, 0);
    r.out_le32(kDma + kOut * DbdmaController::kChannelStride + 0x0C, 0x1000);
    r.out_le32(kDma + kOut * DbdmaController::kChannelStride, ((RUN | WAKE) << 16) | RUN | WAKE);
    r.sched.advance_to(200 * kMs);
    CHECK_EQ(r.sink.frames(), 512u);
    CHECK_EQ(r.sched.pending_count(), 0u);
    // Input: a ring of 1 KB periods fills at 44.1 kHz with zeros.
    for (u32 i = 0x20000; i < 0x24000; ++i)
        r.ram[i] = 0x55;
    int in_irqs = 0;
    r.dma.channel(kIn).set_irq([](void* c) { ++*static_cast<int*>(c); }, &in_irqs);
    r.ring(kIn, 0x3000, 0x20000, 16, 1024);
    r.sched.advance_to(300 * kMs);  // 100 ms: 17640 bytes, 17 periods
    CHECK(in_irqs >= 16);
    CHECK(in_irqs <= 18);
    for (u32 i = 0x20000; i < 0x20000 + 16 * 1024; ++i)
        REQUIRE(r.ram[i] == 0u);
}

TEST_CASE("devices/davbus/screamer_frame_clock_as_mac_os_probes_it")
{
    // Mac OS 9.2.1 (seen in a trace): LOAD_WORD of the frame count register
    // (key 6, system space), output buffers of 176 bytes (1 ms), then it
    // compares. Here: LOAD_WORD, 20 buffers, LOAD_WORD, STOP.
    Rig r;
    // The clock runs without any DMA.
    const u32 f0 = r.snd.frame_count();
    r.sched.advance_to(100 * kMs);
    CHECK_EQ(r.snd.frame_count() - f0, 4410u);
    CHECK_EQ(r.in_le32(kSnd + 0x50), r.snd.frame_count());
    constexpr u32 kBufs = 20;
    r.desc(0x1000, cmd(LOAD_WORD) | (6u << 8), 4, kSnd + 0x50);
    for (u32 i = 0; i < kBufs; ++i)
        r.desc(0x1010 + 16 * i, cmd(OUTPUT_LAST), 176, 0x10000 + 176 * i);
    r.desc(0x1010 + 16 * kBufs, cmd(LOAD_WORD) | (6u << 8), 4, kSnd + 0x50);
    r.desc(0x1020 + 16 * kBufs, cmd(STOP), 0, 0);
    const u32 regs = kDma + kOut * DbdmaController::kChannelStride;
    r.out_le32(regs + 0x0C, 0x1000);
    r.out_le32(regs, ((RUN | WAKE) << 16) | RUN | WAKE);
    // Right after the start, the transfer is still going (it takes 20 ms).
    CHECK(r.in_le32(regs + 4) & ACTIVE);
    r.sched.advance_to(200 * kMs);
    CHECK_EQ(r.in_le32(regs + 4) & ACTIVE, 0u);
    const u32 before = load_le32(&r.ram[0x1000 + 8]);
    const u32 after = load_le32(&r.ram[0x1010 + 16 * kBufs + 8]);
    // 880 frames went out between the two loads: the clock moved as much
    // (to within the FIFO and one interval).
    const u32 moved = after - before;
    CHECK(moved + 16 + 2 >= kBufs * 44);  // the FIFO filled ahead, and rounding
    CHECK(moved <= kBufs * 44 + MacioDavbus::kTickFrames + 16);
    CHECK_EQ(r.sink.frames(), kBufs * 44u);
    // A new rate: the clock continues from where it was, at that rate.
    const u32 c1 = r.snd.frame_count();
    r.out_le32(kSnd + 0x00, 5u << 8);  // 11025 Hz
    r.sched.advance_to(300 * kMs);
    CHECK_EQ(r.snd.frame_count() - c1, 1102u);
}

namespace {
// Linux burgundy.c's helpers, in short: write a 4-byte register byte by
// byte, write a 1-byte register, read a byte (waiting for "extend").
void bwcw(Rig& r, u32 addr, u32 val)
{
    for (u32 i = 0; i < 4; ++i)
        r.out_le32(kSnd + 0x10, (addr << 12) + 0x200c00 + (i << 8) + ((val >> (8 * i)) & 0xFF));
}
void bwcb(Rig& r, u32 addr, u32 val) { r.out_le32(kSnd + 0x10, (addr << 12) + 0x300000 + (val & 0xFF)); }
u32 brcb(Rig& r, u32 addr, u32 byte = 0)
{
    r.out_le32(kSnd + 0x10, (addr << 12) + 0x100000 + (byte << 8));
    CHECK_EQ(r.in_le32(kSnd + 0x10) & (1u << 24), 0u);  // not busy
    u32 n = 0;
    while (!(r.in_le32(kSnd + 0x20) & (1u << 23)) && n < 50)
        ++n;  // extend set...
    CHECK(n < 50);
    u32 st;
    n = 0;
    while (((st = r.in_le32(kSnd + 0x20)) & (1u << 23)) && n < 50)
        ++n;  // ...then clear
    CHECK(n < 50);
    return (st >> 4) & 0xFF;
}
} // namespace

TEST_CASE("devices/davbus/burgundy_registers_as_linux_uses_them")
{
    Rig r;
    r.snd.set_codec(MacioDavbus::Codec::Burgundy);
    // snd_pmac_burgundy_init: refused if codec control's error code is 0xF.
    CHECK((r.in_le32(kSnd + 0x10) & 0xF0000) != 0xF0000);
    bwcw(r, 0x2F, 0x0100000A);  // OUTPUTENABLES
    bwcb(r, 0x60, 0x7E);        // MORE_OUTPUTENABLES
    bwcw(r, 0x30, 0xDDDDDDDD);  // MASTER_VOLUME
    bwcb(r, 0x62, 0x44);        // ATTENSPEAKER
    CHECK_EQ(r.snd.burgundy_reg(0x2F, 0), 0x0Au);
    CHECK_EQ(r.snd.burgundy_reg(0x2F, 3), 0x01u);
    CHECK_EQ(r.snd.burgundy_reg(0x60, 0), 0x7Eu);
    CHECK_EQ(brcb(r, 0x60), 0x7Eu);
    CHECK_EQ(brcb(r, 0x62), 0x44u);
    // snd_pmac_burgundy_rcw: a 4-byte register byte by byte.
    u32 w = 0;
    for (u32 i = 0; i < 4; ++i)
        w |= brcb(r, 0x2F, i) << (8 * i);
    CHECK_EQ(w, 0x0100000Au);
    // Outputs on: samples pass; all off: silence.
    u32 gl, gr;
    r.snd.gains(gl, gr);
    CHECK_EQ(gl, 32768u);
    CHECK_EQ(gr, 32768u);
    bwcb(r, 0x60, 0x00);
    r.snd.gains(gl, gr);
    CHECK_EQ(gl + gr, 0u);
    bwcb(r, 0x60, 0x80);  // the internal speaker only
    r.snd.gains(gl, gr);
    CHECK_EQ(gl, 32768u);
}
