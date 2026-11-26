// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the framebuffer device, through the bus as guest code uses it.

#include "core/devices/video/framebuffer.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;

namespace {
constexpr u32 kVram = 0x90000000u, kRegs = 0x91000000u;
struct Rig {
    std::vector<u8> vram = std::vector<u8>(4u << 20);
    PhysicalBus bus;
    Framebuffer fb{vram.data(), static_cast<u32>(vram.size())};
    Rig() { REQUIRE_OK(fb.map(bus, kVram, kRegs)); }
    static void REQUIRE_OK(bool ok) { if (!ok) __builtin_trap(); }
    u32 rd(u32 reg) { u32 v = 0; bus.read32(kRegs + reg, v); return v; }
    void wr(u32 reg, u32 v) { bus.write32(kRegs + reg, v); }
    bool mode(u32 w, u32 h, u32 d)
    {
        wr(Framebuffer::WIDTH, w); wr(Framebuffer::HEIGHT, h); wr(Framebuffer::DEPTH, d);
        wr(Framebuffer::APPLY, 1);
        return rd(Framebuffer::STATUS) == 1;
    }
};
} // namespace

TEST_CASE("devices/framebuffer/registers_and_modes")
{
    Rig r;
    CHECK_EQ(r.rd(Framebuffer::ID), Framebuffer::kId);
    u8 b = 0;
    r.bus.read8(kRegs + 0, b);
    CHECK_EQ(b, static_cast<u32>('G'));                       // big-endian bytes
    u16 h = 0;
    r.bus.read16(kRegs + 2, h);
    CHECK_EQ(h, 0x4642u);                                    // "FB"
    CHECK_EQ(r.rd(Framebuffer::VERSION), 2u);
    CHECK_EQ(r.rd(Framebuffer::VRAMSIZE), 4u << 20);
    r.wr(Framebuffer::ID, 0);                                // read-only
    CHECK_EQ(r.rd(Framebuffer::ID), Framebuffer::kId);

    REQUIRE(r.mode(640, 480, 8));
    CHECK_EQ(r.fb.width(), 640u);
    CHECK_EQ(r.rd(Framebuffer::PITCH), 640u);
    REQUIRE(r.mode(1000, 700, 15));
    CHECK_EQ(r.rd(Framebuffer::PITCH), 2016u);               // 2000 rounded up to 32
    REQUIRE(r.mode(1024, 768, 32));
    CHECK_EQ(r.rd(Framebuffer::PITCH), 4096u);
    // Rejected modes leave the active mode alone.
    CHECK(!r.mode(1280, 1024, 32));                          // 5 MB > 4 MB of VRAM
    CHECK(!r.mode(800, 600, 24));                            // unsupported depth
    CHECK(!r.mode(0, 600, 8));
    CHECK(!r.mode(4000, 100, 8));                            // wider than kMaxWidth
    CHECK_EQ(r.fb.width(), 1024u);
    CHECK_EQ(r.fb.depth(), 32u);
    CHECK_EQ(r.rd(Framebuffer::WIDTH), 4000u);               // staged value reads back
    CHECK_EQ(r.rd(Framebuffer::PITCH), 4096u);
    r.wr(Framebuffer::ENABLE, 1);
    CHECK(r.fb.enabled());
    // Palette: auto-incrementing index for writes and reads.
    r.wr(Framebuffer::PALINDEX, 254);
    r.wr(Framebuffer::PALDATA, 0xFF112233u);                 // top byte ignored
    r.wr(Framebuffer::PALDATA, 0x00445566u);
    r.wr(Framebuffer::PALDATA, 0x00778899u);                 // wraps to index 0
    CHECK_EQ(r.fb.palette(254), 0x112233u);
    CHECK_EQ(r.fb.palette(255), 0x445566u);
    CHECK_EQ(r.fb.palette(0), 0x778899u);
    CHECK_EQ(r.rd(Framebuffer::PALINDEX), 1u);
    r.wr(Framebuffer::PALINDEX, 255);
    CHECK_EQ(r.rd(Framebuffer::PALDATA), 0x445566u);
    CHECK_EQ(r.rd(Framebuffer::PALDATA), 0x778899u);
    // Overlapping mappings are refused.
    std::vector<u8> v2(4096);
    Framebuffer other(v2.data(), 4096);
    CHECK(!other.map(r.bus, kVram + 0x1000, 0x92000000u));
    CHECK(!other.map(r.bus, 0x93000000u, kRegs));
}

TEST_CASE("devices/framebuffer/pixels_written_by_the_cpu_convert")
{
    Rig r;
    std::vector<u32> out(4 * 2);
    // 8 bits: palette lookup.
    REQUIRE(r.mode(4, 2, 8));
    r.wr(Framebuffer::PALINDEX, 7);
    r.wr(Framebuffer::PALDATA, 0x00ABCDEFu);
    r.bus.write8(kVram + 1, 7);                              // pixel (1,0)
    r.bus.write8(kVram + r.fb.pitch() + 3, 255);             // pixel (3,1): grey-ramp white
    r.fb.convert(out.data(), 4);
    CHECK_EQ(out[1], 0xABCDEFu);
    CHECK_EQ(out[0], 0u);                                    // index 0 = black
    CHECK_EQ(out[4 + 3], 0xFFFFFFu);
    // 15: x1R5G5B5 big-endian, 5 bits expanded to 8.
    REQUIRE(r.mode(4, 2, 15));
    r.bus.write16(kVram + 0, 0x7C00);                        // pure red
    r.bus.write16(kVram + 2, 0x03E0);                        // pure green
    r.bus.write16(kVram + 4, 0x001F);                        // pure blue
    r.bus.write16(kVram + 6, 0x8000 | (16 << 10) | (8 << 5) | 1);  // top bit ignored
    r.fb.convert(out.data(), 4);
    CHECK_EQ(out[0], 0xFF0000u);
    CHECK_EQ(out[1], 0x00FF00u);
    CHECK_EQ(out[2], 0x0000FFu);
    CHECK_EQ(out[3], (0x84u << 16) | (0x42u << 8) | 0x08u);
    // 32: x8R8G8B8 big-endian.
    REQUIRE(r.mode(4, 2, 32));
    r.bus.write32(kVram + r.fb.pitch() + 8, 0xEE123456u);    // pixel (2,1), top byte ignored
    r.fb.convert(out.data(), 4);
    CHECK_EQ(out[4 + 2], 0x123456u);
    CHECK_EQ(r.rd(Framebuffer::FRAMES), 3u);
    // A destination wider than the picture: rows land at dst_pitch.
    std::vector<u32> wide(10 * 2, 0xDEAD);
    r.fb.convert(wide.data(), 10);
    CHECK_EQ(wide[10 + 2], 0x123456u);
    CHECK_EQ(wide[5], 0xDEADu);                              // beyond the width: untouched
}

TEST_CASE("devices/framebuffer/vertical_blank_interrupt")
{
    Rig r;
    Scheduler sched;
    struct Line { bool up = false; u32 rises = 0; } line;
    r.fb.attach_interrupt(sched, [](void* c, bool a) {
        auto* l = static_cast<Line*>(c);
        if (a && !l->up)
            ++l->rises;
        l->up = a;
    }, &line);
    // Off: no events, nothing pending.
    sched.advance_to(Framebuffer::kVblPs * 3);
    CHECK_EQ(sched.pending_count(), 0u);
    CHECK_EQ(r.rd(Framebuffer::IRQ_STATUS), 0u);
    CHECK(!line.up);
    // On: a vertical blank every 1/60 s raises the line until acknowledged.
    const u64 t0 = sched.now();
    r.wr(Framebuffer::IRQ_ENABLE, 1);
    CHECK_EQ(r.rd(Framebuffer::IRQ_ENABLE), 1u);
    sched.advance_to(t0 + Framebuffer::kVblPs - 1);
    CHECK(!line.up);
    sched.advance_to(t0 + Framebuffer::kVblPs);
    CHECK(line.up);
    CHECK_EQ(r.rd(Framebuffer::IRQ_STATUS), 1u);
    r.wr(Framebuffer::IRQ_STATUS, 0);                  // writing 0 clears nothing
    CHECK(line.up);
    r.wr(Framebuffer::IRQ_STATUS, 1);                  // write 1 to clear
    CHECK(!line.up);
    CHECK_EQ(r.rd(Framebuffer::IRQ_STATUS), 0u);
    // 60 a second, at a steady rate.
    sched.advance_to(t0 + Framebuffer::kVblPs * 60);
    CHECK_EQ(line.rises, 2u);                          // not acknowledged since: stays high
    for (u32 i = 61; i <= 120; ++i) {
        r.wr(Framebuffer::IRQ_STATUS, 1);
        sched.advance_to(t0 + Framebuffer::kVblPs * i);
    }
    CHECK_EQ(line.rises, 62u);
    // Turned off while pending: the line drops at once and the timer stops.
    sched.advance_to(t0 + Framebuffer::kVblPs * 121);
    CHECK(line.up);
    r.wr(Framebuffer::IRQ_ENABLE, 0);
    CHECK(!line.up);
    CHECK_EQ(sched.pending_count(), 0u);
    r.wr(Framebuffer::IRQ_STATUS, 1);
    sched.advance_to(t0 + Framebuffer::kVblPs * 200);
    CHECK(!line.up);
    CHECK_EQ(r.rd(Framebuffer::IRQ_STATUS), 0u);
    // On again: the next blank a full period later.
    const u64 t1 = sched.now();
    r.wr(Framebuffer::IRQ_ENABLE, 1);
    sched.advance_to(t1 + Framebuffer::kVblPs - 1);
    CHECK(!line.up);
    sched.advance_to(t1 + Framebuffer::kVblPs);
    CHECK(line.up);
}
