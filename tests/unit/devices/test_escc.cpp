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

TEST_CASE("devices/escc/receive_fifo_and_overrun")
{
    MacioEscc e;
    CHECK(!e.rx_ready(MacioEscc::A));                        // receiver off
    e.rx_push(MacioEscc::A, 'x');
    CHECK_EQ(rd(e, kACtl) & 0x01, 0u);                       // dropped: nothing assembled
    wreg(e, kACtl, 3, 0xC1);                                 // 8 bits, Rx enable
    CHECK(e.rx_ready(MacioEscc::A));
    e.rx_push(MacioEscc::A, 'a');
    e.rx_push(MacioEscc::A, 'b');
    e.rx_push(MacioEscc::A, 'c');
    CHECK(!e.rx_ready(MacioEscc::A));                        // 3 held
    e.rx_push(MacioEscc::A, 'd');                            // lost
    CHECK_EQ(rreg(e, kACtl, 1) & 0x20, 0x20u);               // overrun
    CHECK_EQ(rd(e, kACtl) & 0x01, 0x01u);
    CHECK_EQ(rd(e, kAData), u32('a'));
    CHECK_EQ(rd(e, kAData), u32('b'));
    CHECK_EQ(rd(e, kAData), u32('c'));
    CHECK_EQ(rd(e, kACtl) & 0x01, 0u);
    wr(e, kACtl, 0x30);                                      // error reset
    CHECK_EQ(rreg(e, kACtl, 1) & 0x20, 0u);
    // Channel B is separate.
    CHECK(!e.rx_ready(MacioEscc::B));
}

TEST_CASE("devices/escc/receive_interrupt_modes")
{
    MacioEscc e;
    bool line[2] = {};
    e.set_irq([](void* c, u32 ch, bool a) { static_cast<bool*>(c)[ch] = a; }, line);
    wreg(e, kACtl, 3, 0xC1);
    wreg(e, kACtl, 9, 0x08);                                 // MIE
    // Every character.
    wreg(e, kACtl, 1, 0x10);
    e.rx_push(MacioEscc::A, '1');
    CHECK(line[0]);
    CHECK_EQ(rreg(e, kACtl, 3), 0x20u);                      // CHARxIP
    e.rx_push(MacioEscc::A, '2');
    rd(e, kAData);
    CHECK(line[0]);                                          // one still held
    rd(e, kAData);
    CHECK(!line[0]);
    // First character only, re-armed by WR0 command 4.
    wreg(e, kACtl, 1, 0x08);
    e.rx_push(MacioEscc::A, '3');
    CHECK(line[0]);
    rd(e, kAData);
    e.rx_push(MacioEscc::A, '4');
    CHECK(!line[0]);                                         // not the first any more
    wr(e, kACtl, 0x20);                                      // RES_RxINT_FC
    CHECK(line[0]);
    rd(e, kAData);
    CHECK(!line[0]);
    // Special conditions only: an overrun.
    wreg(e, kACtl, 1, 0x18);
    for (int i = 0; i < 3; ++i)
        e.rx_push(MacioEscc::A, 'x');
    CHECK(!line[0]);
    e.rx_push(MacioEscc::A, 'y');
    CHECK(line[0]);
    wr(e, kACtl, 0x30);
    CHECK(!line[0]);
    // Channel B's Rx pending bit is RR3 bit 2.
    wreg(e, kBCtl, 3, 0xC1);
    wreg(e, kBCtl, 1, 0x10);
    e.rx_push(MacioEscc::B, 'z');
    CHECK(line[1]);
    CHECK_EQ(rreg(e, kACtl, 3) & 0x04, 0x04u);
}

TEST_CASE("devices/escc/modem_lines")
{
    MacioEscc e;
    struct Seen { int calls = 0; bool dtr = false, rts = false; } seen;
    e.set_lines_callback([](void* c, u32 ch, bool d, bool r) {
        if (ch != MacioEscc::A) return;
        auto* s = static_cast<Seen*>(c);
        ++s->calls; s->dtr = d; s->rts = r;
    }, &seen);
    bool line[2] = {};
    e.set_irq([](void* c, u32 ch, bool a) { static_cast<bool*>(c)[ch] = a; }, line);
    wreg(e, kACtl, 5, 0xEA);                                 // DTR, 8 bits, Tx enable, RTS
    CHECK_EQ(seen.calls, 1);
    CHECK(seen.dtr);
    CHECK(seen.rts);
    wreg(e, kACtl, 5, 0x6A);                                 // DTR off
    CHECK_EQ(seen.calls, 2);
    CHECK(!seen.dtr);
    wreg(e, kACtl, 5, 0x6A);                                 // no change: not reported
    CHECK_EQ(seen.calls, 2);
    // DCD and CTS in RR0; a DCD change interrupts with WR1 ext + WR15 DCDIE.
    CHECK_EQ(rd(e, kACtl) & 0x28, 0u);
    e.set_dcd(MacioEscc::A, true);
    CHECK_EQ(rd(e, kACtl) & 0x08, 0x08u);
    CHECK(!line[0]);                                         // not enabled
    wreg(e, kACtl, 15, 0x08);
    wreg(e, kACtl, 1, 0x01);
    wreg(e, kACtl, 9, 0x08);
    e.set_dcd(MacioEscc::A, false);
    CHECK(line[0]);
    CHECK_EQ(rreg(e, kACtl, 3) & 0x08, 0x08u);               // CHAEXT
    wr(e, kACtl, 0x10);                                      // RES_EXT_INT
    CHECK(!line[0]);
    e.set_cts(MacioEscc::A, true);                           // CTSIE not set
    CHECK(!line[0]);
    CHECK_EQ(rd(e, kACtl) & 0x20, 0x20u);
    // Each source has its own enable: with only CTSIE, DCD changes are quiet.
    wreg(e, kACtl, 15, 0x20);
    e.set_dcd(MacioEscc::A, true);
    CHECK(!line[0]);
    e.set_cts(MacioEscc::A, false);
    CHECK(line[0]);
    wr(e, kACtl, 0x10);
    // A hardware reset drops DTR and RTS (reported).
    wreg(e, kACtl, 5, 0x82);
    wreg(e, kACtl, 9, 0xC0);
    CHECK(!seen.dtr);
    CHECK(!seen.rts);
}

TEST_CASE("devices/escc/baud_rate")
{
    MacioEscc e;
    CHECK_EQ(e.baud(MacioEscc::A), 0u);                      // generator off
    wreg(e, kACtl, 4, 0x44);                                 // x16 clock, 1 stop bit
    wreg(e, kACtl, 12, 0);
    wreg(e, kACtl, 13, 0);
    wreg(e, kACtl, 14, 0x01);                                // BRG enable
    CHECK_EQ(e.baud(MacioEscc::A), 57600u);                  // 3686400 / (2 * 16 * 2)
    wreg(e, kACtl, 12, 4);
    CHECK_EQ(e.baud(MacioEscc::A), 19200u);
}
