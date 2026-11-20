// SPDX-License-Identifier: GPL-3.0-or-later
// The ESCC (Z85C30) with nothing connected (escc.h): register pointer and
// "point high", both address layouts, resets, transmit and its interrupt
// through WR9's master enable, and Paddington's inputs 15 and 16.

#include "core/devices/macio/escc.h"
#include "core/machine/yosemite.h"
#include "tests/framework/g3test.h"

#include <string>

using namespace g3;

namespace {
// MacRISC view (window + 0x1000): B control 0x00, B data 0x10, A control 0x20, A data 0x30.
constexpr u32 kBCtl = 0x1000, kBData = 0x1010, kACtl = 0x1020, kAData = 0x1030;
u8 rd(MacioEscc& e, u32 off) { return static_cast<u8>(e.mmio_read(off, 1)); }
void wr(MacioEscc& e, u32 off, u8 v) { e.mmio_write(off, 1, v); }
void wreg(MacioEscc& e, u32 ctl, u32 n, u8 v)
{
    wr(e, ctl, static_cast<u8>((n & 7) | (n >= 8 ? 0x08 : 0)));   // select (+ point high)
    wr(e, ctl, v);
}
u8 rreg(MacioEscc& e, u32 ctl, u32 n)
{
    wr(e, ctl, static_cast<u8>((n & 7) | (n >= 8 ? 0x08 : 0)));
    return rd(e, ctl);
}
} // namespace

TEST_CASE("devices/escc/registers_and_layouts")
{
    MacioEscc e;
    // RR0 with nothing connected: Tx buffer empty, nothing received.
    CHECK_EQ(rd(e, kACtl) & 0x05, 0x04u);
    CHECK_EQ(rreg(e, kACtl, 1) & 0x01, 0x01u);              // all sent
    // Write registers through the pointer; point high reaches WR8-15.
    wreg(e, kACtl, 12, 0x0A);
    wreg(e, kACtl, 13, 0x00);
    wreg(e, kBCtl, 12, 0x34);
    CHECK_EQ(e.wr(MacioEscc::A, 12), 0x0Au);
    CHECK_EQ(e.wr(MacioEscc::B, 12), 0x34u);
    CHECK_EQ(rreg(e, kACtl, 12), 0x0Au);                    // RR12 reads WR12 (baud constant)
    // The pointer returns to 0 after each access.
    CHECK_EQ(rd(e, kACtl) & 0x04, 0x04u);
    // WR2 is the shared vector, read back through RR2.
    wreg(e, kACtl, 2, 0x5A);
    CHECK_EQ(rreg(e, kBCtl, 2), 0x5Au);
    // Legacy view: B control +0, A control +2, B data +4, A data +6.
    wr(e, 0x2, 0x0C);                                        // select WR12 on channel A
    wr(e, 0x2, 0x77);
    CHECK_EQ(e.wr(MacioEscc::A, 12), 0x77u);
    wr(e, 0x0, 0x0C);
    wr(e, 0x0, 0x66);
    CHECK_EQ(e.wr(MacioEscc::B, 12), 0x66u);
    // Odd and out-of-range offsets are not ports.
    wr(e, 0x1, 0x0C);
    wr(e, 0x1004, 0x0C);
    CHECK_EQ(e.wr(MacioEscc::A, 12), 0x77u);
    // WR9 resets: channel A only, then both.
    wreg(e, kACtl, 9, 0x80);
    CHECK_EQ(e.wr(MacioEscc::A, 12), 0u);
    CHECK_EQ(e.wr(MacioEscc::B, 12), 0x66u);
    wreg(e, kACtl, 9, 0xC0);
    CHECK_EQ(e.wr(MacioEscc::B, 12), 0u);
}

TEST_CASE("devices/escc/transmit_and_interrupts")
{
    MacioEscc e;
    std::string sent;
    e.set_tx_sink([](void* c, u32 ch, u8 b) { if (ch == MacioEscc::A) static_cast<std::string*>(c)->push_back(char(b)); },
                  &sent);
    bool line[2] = {};
    e.set_irq([](void* c, u32 ch, bool a) { static_cast<bool*>(c)[ch] = a; }, line);
    wr(e, kAData, 'h');
    wr(e, kAData, 'i');
    CHECK(sent == "hi");
    CHECK(!line[0]);                                         // Tx interrupts not enabled
    wreg(e, kACtl, 1, 0x02);                                 // Tx interrupt enable
    wr(e, kAData, '!');
    CHECK(!line[0]);                                         // pending, but MIE off
    CHECK_EQ(rreg(e, kACtl, 3), 0x10u);                      // RR3: channel A Tx IP
    wreg(e, kACtl, 9, 0x08);                                 // master interrupt enable
    CHECK(line[0]);
    CHECK(!line[1]);
    wr(e, kACtl, 0x28);                                      // WR0 command 5: reset Tx int pending
    CHECK(!line[0]);
    CHECK_EQ(rreg(e, kACtl, 3), 0u);
    // Channel B's pending bit is RR3 bit 1, read through channel A only.
    wreg(e, kBCtl, 1, 0x02);
    wr(e, kBData, 'x');
    CHECK(line[1]);
    CHECK_EQ(rreg(e, kACtl, 3), 0x02u);
    CHECK_EQ(rreg(e, kBCtl, 3), 0u);
}

TEST_CASE("devices/escc/in_paddington")
{
    static u8 ram[64 * 1024];
    YosemiteConfig cfg;
    cfg.ram = ram;
    cfg.ram_size = sizeof ram;
    YosemiteMachine m(cfg);
    REQUIRE(m.ok());
    Paddington& p = m.macio();
    p.config_write(0x10, 0x80800000u, 0xF);
    p.config_write(0x04, 0x2, 0x3);
    // The legacy port Mac OS read (a machine check before), and Linux's ttyPZ0.
    u8 v = 0;
    REQUIRE(m.bus().read8(0x80812000u, v));
    CHECK_EQ(v & 0x04, 0x04u);
    REQUIRE(m.bus().read8(0x80813020u, v));
    CHECK_EQ(v & 0x04, 0x04u);
    // Channel A Tx interrupt on PIC input 15 (bank 0 bit 15).
    p.pic().mmio_write(0x24, 4, 0x00800000u);               // enable input 15 (little-endian register)
    REQUIRE(m.bus().write8(0x80813020u, 0x01));
    REQUIRE(m.bus().write8(0x80813020u, 0x02));             // WR1: Tx interrupt enable
    REQUIRE(m.bus().write8(0x80813020u, 0x09));
    REQUIRE(m.bus().write8(0x80813020u, 0x08));             // WR9: MIE
    REQUIRE(m.bus().write8(0x80813030u, 'A'));
    CHECK(p.pic().output());
}
