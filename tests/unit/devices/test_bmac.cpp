// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/devices/test_bmac.cpp — the BMAC+ Ethernet cell, driven the
// way Linux's bmac driver does: 16-bit little-endian registers, MII by bit
// banging MIFCSR, a transmit ring of OUTPUT_LAST descriptors and a receive
// ring of INPUT_LAST descriptors whose length is the residual count.

#include "core/common/endian.h"
#include "core/devices/macio/bmac.h"
#include "core/sched/scheduler.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <deque>
#include <vector>

using namespace g3;
using namespace g3::dbdma;

namespace {

constexpr u32 kDma = 0x80808000u, kEth = 0x80811000u;
constexpr u32 kTx = 0x200 / 0x80, kRx = 0x300 / 0x80;
constexpr u64 kMs = 1000000000ull;
constexpr u32 kRxBuf = 1520;  // Linux RX_BUFLEN
const u8 kMe[6] = {0x02, 0x47, 0x33, 0x4D, 0x50, 0x01};

struct Wire final : NetLink {
    std::vector<std::vector<u8>> sent;
    std::deque<std::vector<u8>> incoming;
    void net_send(const u8* f, u32 n) override { sent.emplace_back(f, f + n); }
    u32 net_receive(u8* buf) override
    {
        if (incoming.empty())
            return 0;
        const std::vector<u8> f = incoming.front();
        incoming.pop_front();
        std::memcpy(buf, f.data(), f.size());
        return static_cast<u32>(f.size());
    }
};

constexpr u16 cmd(u16 c, u16 i = 0, u16 b = 0, u16 w = 0)
{
    return static_cast<u16>((c << 12) | (i << 4) | (b << 2) | w);
}

std::vector<u8> frame(const u8* dst, const u8* src, u32 payload, u8 seed)
{
    std::vector<u8> f(dst, dst + 6);
    f.insert(f.end(), src, src + 6);
    f.push_back(0x08);
    f.push_back(0x00);
    for (u32 i = 0; i < payload; ++i)
        f.push_back(static_cast<u8>(seed + i * 3));
    return f;
}

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x80000);
    PhysicalBus bus;
    Scheduler sched;
    DbdmaController dma{bus};
    MacioBmac eth{sched};
    Wire wire;
    bool line = false;
    int tx_irqs = 0, rx_irqs = 0;
    Rig()
    {
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        bus.map_mmio(kDma, DbdmaController::kWindowSize, &dma);
        bus.map_mmio(kEth, MacioBmac::kWindowSize, &eth);
        eth.attach_dma(&dma.channel(kTx), &dma.channel(kRx));
        eth.set_link(&wire);
        eth.set_irq([](void* c, bool a) { *static_cast<bool*>(c) = a; }, &line);
        dma.channel(kTx).set_irq([](void* c) { ++*static_cast<int*>(c); }, &tx_irqs);
        dma.channel(kRx).set_irq([](void* c) { ++*static_cast<int*>(c); }, &rx_irqs);
    }
    void w16(u32 reg, u16 v) { bus.write16(kEth + reg, byteswap16(v)); }
    u16 r16(u32 reg)
    {
        u16 v = 0;
        bus.read16(kEth + reg, v);
        return byteswap16(v);
    }
    void out_le32(u32 a, u32 v) { bus.write32(a, byteswap32(v)); }
    u32 in_le32(u32 a)
    {
        u32 v = 0;
        bus.read32(a, v);
        return byteswap32(v);
    }
    void desc(u32 at, u16 c, u16 req, u32 addr, u32 dep = 0)
    {
        store_le16(&ram[at + 0], req);
        store_le16(&ram[at + 2], c);
        store_le32(&ram[at + 4], addr);
        store_le32(&ram[at + 8], dep);
        store_le32(&ram[at + 12], 0);
    }
    u32 chan(u32 ch) { return kDma + ch * DbdmaController::kChannelStride; }

    // --- MII as bmac.c bit-bangs it
    void mif_bits(u32 v, u32 n)
    {
        for (u32 i = n; i-- > 0;) {
            const u16 b = ((v >> i) & 1) ? 6 : 4;
            w16(MacioBmac::MIFCSR, b);
            w16(MacioBmac::MIFCSR, b | 1);
        }
    }
    u16 mif_read(u32 reg)
    {
        w16(MacioBmac::MIFCSR, 4);
        mif_bits(~0u, 32);
        mif_bits(6, 4);
        mif_bits(reg, 10);
        w16(MacioBmac::MIFCSR, 2);
        w16(MacioBmac::MIFCSR, 1);
        u32 v = 0;
        for (u32 i = 0; i < 17; ++i) {
            w16(MacioBmac::MIFCSR, 0);
            v = (v << 1) | ((r16(MacioBmac::MIFCSR) & 8) ? 1 : 0);
            w16(MacioBmac::MIFCSR, 1);
        }
        w16(MacioBmac::MIFCSR, 4);
        return static_cast<u16>(v);
    }
    void mif_write(u32 reg, u16 data)
    {
        w16(MacioBmac::MIFCSR, 4);
        mif_bits(~0u, 32);
        mif_bits(5, 4);
        mif_bits(reg, 10);
        mif_bits(2, 2);
        mif_bits(data, 16);
        mif_bits(3, 2);
    }

    // bmac_init_registers + bmac_start_chip, in short.
    void init(u16 rxcfg = 0x0B00)
    {
        w16(0x620, 0);
        w16(0x420, 1);
        CHECK_EQ(r16(0x420) & 1, 0);  // the reset finishes
        w16(0x100, 1);
        w16(0x120, 1);
        (void)r16(MacioBmac::STATUS);
        w16(MacioBmac::MADD0, static_cast<u16>((kMe[0] << 8) | kMe[1]));
        w16(MacioBmac::MADD1, static_cast<u16>((kMe[2] << 8) | kMe[3]));
        w16(MacioBmac::MADD2, static_cast<u16>((kMe[4] << 8) | kMe[5]));
        w16(MacioBmac::RXCFG, rxcfg);
        w16(MacioBmac::INTDISABLE, static_cast<u16>(~(MacioBmac::FrameReceived | MacioBmac::FrameSent)));
        w16(MacioBmac::TXCFG, MacioBmac::TxMACEnable);
        w16(MacioBmac::RXCFG, static_cast<u16>(rxcfg | MacioBmac::RxMACEnable));
    }
    // A receive ring of n INPUT_LAST + INTR_ALWAYS, then NOP + BR_ALWAYS.
    void rx_ring(u32 cmds, u32 bufs, u32 n, u16 input = INPUT_LAST, u16 buf_len = kRxBuf)
    {
        for (u32 i = 0; i < n; ++i)
            desc(cmds + 16 * i, cmd(input, 3), buf_len, bufs + i * 2048);
        desc(cmds + 16 * n, cmd(NOP, 0, 3), 0, 0, cmds);
        out_le32(chan(kRx) + 0x0C, cmds);
        out_le32(chan(kRx), ((RUN | WAKE) << 16) | RUN | WAKE);
    }
};

u32 crc32(const u8* p, size_t n)
{
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
    }
    return ~c;
}

} // namespace

TEST_CASE("devices/bmac/registers_and_phy")
{
    Rig r;
    CHECK_EQ(r.r16(MacioBmac::CHIPID), 0xC7u);
    CHECK_EQ(r.r16(MacioBmac::INTDISABLE), 0xFFFFu);  // everything off at reset
    r.w16(MacioBmac::MADD0, 0x1234);
    CHECK_EQ(r.r16(MacioBmac::MADD0), 0x1234u);
    // The PHY, as bmac_init_phy reads it.
    CHECK_EQ(r.mif_read(2), 0x7810u);
    CHECK_EQ(r.mif_read(1) & 0x0024, 0x0024u);  // link up, negotiation done
    CHECK_EQ(r.mif_read(4), 0x01E1u);
    r.mif_write(4, 0x0061);
    CHECK_EQ(r.mif_read(4), 0x0061u);
    r.mif_write(0, 0x9200);                     // reset + restart negotiation
    CHECK_EQ(r.mif_read(0), 0x1200u);           // reset bit clears itself
    CHECK_EQ(r.mif_read(2), 0x7810u);           // ID registers are read-only
    r.mif_write(2, 0);
    CHECK_EQ(r.mif_read(2), 0x7810u);
    // Another PHY address: nobody drives the line.
    CHECK_EQ(r.mif_read((1u << 5) | 2), 0xFFFFu);
}

TEST_CASE("devices/bmac/transmit_ring")
{
    Rig r;
    r.init();
    const u8 bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // bmac_construct_xmt: OUTPUT_LAST | INTR_ALWAYS | WAIT_IFCLR, then a STOP sentinel.
    const std::vector<u8> a = frame(bc, kMe, 50, 1), b = frame(bc, kMe, 1000, 9);
    std::memcpy(&r.ram[0x10000], a.data(), a.size());
    std::memcpy(&r.ram[0x11000], b.data(), b.size());
    r.out_le32(r.chan(kTx) + 0x18, 0x00200020);  // wait_sel, as bmac_init_tx_ring
    r.desc(0x1000, cmd(OUTPUT_LAST, 3, 0, 2), static_cast<u16>(a.size()), 0x10000);
    r.desc(0x1010, cmd(OUTPUT_LAST, 3, 0, 2), static_cast<u16>(b.size()), 0x11000);
    r.desc(0x1020, cmd(STOP), 0, 0);
    r.out_le32(r.chan(kTx) + 0x0C, 0x1000);
    r.out_le32(r.chan(kTx), ((RUN | WAKE) << 16) | RUN | WAKE);
    REQUIRE(r.wire.sent.size() == 2u);
    CHECK(r.wire.sent[0] == a);
    CHECK(r.wire.sent[1] == b);
    CHECK_EQ(r.tx_irqs, 2);
    CHECK(r.in_le32(0x1000 + 12) >> 16 & ACTIVE);  // xfer_status written back
    CHECK(r.line);                                 // FrameSent, enabled
    CHECK(r.r16(MacioBmac::STATUS) & MacioBmac::FrameSent);
    CHECK(!r.line);                                // reading cleared it
    CHECK_EQ(r.r16(MacioBmac::STATUS), 0u);
    // Transmitter off: the frame does not go out.
    r.w16(MacioBmac::TXCFG, 0);
    r.desc(0x1020, cmd(OUTPUT_LAST, 3), static_cast<u16>(a.size()), 0x10000);
    r.desc(0x1030, cmd(STOP), 0, 0);
    r.out_le32(r.chan(kTx), (WAKE << 16) | WAKE);
    CHECK_EQ(r.wire.sent.size(), 2u);
    CHECK_EQ(r.eth.frames_dropped(), 1u);
}

TEST_CASE("devices/bmac/receive_ring_length_from_residual")
{
    Rig r;
    r.init();  // RxCRCNoStrip | hash filter | reject own, as Linux
    r.rx_ring(0x2000, 0x20000, 8);
    const u8 other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const u8 bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const u8 mc[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    const std::vector<u8> to_me = frame(kMe, other, 200, 3);  // 214 bytes
    const std::vector<u8> tiny = frame(bc, other, 10, 5);     // 24 bytes: padded to 60
    const std::vector<u8> elsewhere = frame(other, other, 100, 7);
    const std::vector<u8> own = frame(bc, kMe, 100, 8);
    const std::vector<u8> multi = frame(mc, other, 100, 9);
    const std::vector<u8> big = frame(kMe, other, 1500, 11);  // 1514 + CRC = 1518 bytes
    for (const auto* f : {&to_me, &elsewhere, &tiny, &own, &multi, &big})
        r.wire.incoming.push_back(*f);
    r.sched.advance_to(5 * kMs);
    CHECK(r.wire.incoming.empty());
    // Delivered: to_me, tiny, big (not: another station, our own, multicast
    // with an empty hash table).
    CHECK_EQ(r.eth.frames_received(), 3u);
    CHECK_EQ(r.rx_irqs, 3);
    const auto check = [&](u32 slot, const std::vector<u8>& f, u32 wire_len) {
        const u32 d = 0x2000 + 16 * slot;
        const u16 res = load_le16(&r.ram[d + 12]), st = load_le16(&r.ram[d + 14]);
        CHECK(st & ACTIVE);
        const u32 nb = kRxBuf - res - 2;  // bmac_rxdma_intr: length from the residual, minus 2
        REQUIRE(nb == wire_len + 4);
        // The status word after the frame (Mac OS reads it): the length.
        CHECK_EQ(static_cast<u32>(r.ram[0x20000 + slot * 2048 + nb] << 8 | r.ram[0x20000 + slot * 2048 + nb + 1]), nb);
        const u8* got = &r.ram[0x20000 + slot * 2048];
        CHECK(std::memcmp(got, f.data(), f.size()) == 0);
        for (u32 i = static_cast<u32>(f.size()); i < wire_len; ++i)
            CHECK_EQ(got[i], 0u);
        const u32 c = crc32(got, wire_len);
        CHECK_EQ(load_le32(got + wire_len), c);  // the frame check sequence, kept
    };
    check(0, to_me, 214);
    check(1, tiny, 60);
    check(2, big, 1514);
    CHECK_EQ(load_le16(&r.ram[0x2000 + 3 * 16 + 14]), 0u);  // the next slot is untouched
    CHECK(r.r16(MacioBmac::STATUS) & MacioBmac::FrameReceived);
}

// Mac OS 9.2.1's driver receives into 1536-byte INPUT_MORE descriptors
// (seen in a trace): each still ends with its frame.
TEST_CASE("devices/bmac/receive_into_input_more_descriptors")
{
    Rig r;
    r.init();
    r.rx_ring(0x2000, 0x20000, 4, INPUT_MORE, 1536);
    const u8 bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const u8 other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const std::vector<u8> a = frame(bc, other, 300, 1), b = frame(kMe, other, 100, 2);
    r.wire.incoming.push_back(a);
    r.wire.incoming.push_back(b);
    r.sched.advance_to(5 * kMs);
    CHECK_EQ(r.eth.frames_received(), 2u);
    CHECK_EQ(r.rx_irqs, 2);
    // As Mac OS reads it: the status word in the last two bytes written,
    // its low 14 bits the length with the CRC, bit 15 clear.
    const auto status = [&](u32 slot, u32 desc) {
        const u32 got = 1536u - load_le16(&r.ram[desc + 12]);
        const u8* p = &r.ram[0x20000 + slot * 2048 + got - 2];
        return static_cast<u32>(p[0] << 8 | p[1]);
    };
    CHECK_EQ(status(0, 0x2000), 314u + 4);
    CHECK_EQ(status(1, 0x2010), 114u + 4);
    CHECK(std::memcmp(&r.ram[0x20000 + 2048], b.data(), b.size()) == 0);
}

TEST_CASE("devices/bmac/receive_waits_for_descriptors_and_filters")
{
    Rig r;
    r.init(0x0100 | 0x0800);  // keep CRC, hash filter, own packets allowed
    const u8 other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const u8 mc[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};  // AppleTalk broadcast
    // No ring yet: frames wait (one in the cell, the rest on the wire).
    for (u8 i = 0; i < 4; ++i)
        r.wire.incoming.push_back(frame(kMe, other, 60 + i, i));
    r.sched.advance_to(5 * kMs);
    CHECK_EQ(r.eth.frames_received(), 0u);
    // A ring of 2: two frames; the third when a slot is given back.
    for (u32 i = 0; i < 2; ++i)
        r.desc(0x3000 + 16 * i, cmd(INPUT_LAST, 3), kRxBuf, 0x30000 + i * 2048);
    r.desc(0x3020, cmd(STOP), 0, 0);
    r.out_le32(r.chan(kRx) + 0x0C, 0x3000);
    r.out_le32(r.chan(kRx), ((RUN | WAKE) << 16) | RUN | WAKE);
    r.sched.advance_to(10 * kMs);
    CHECK_EQ(r.eth.frames_received(), 2u);
    r.desc(0x3020, cmd(INPUT_LAST, 3), kRxBuf, 0x34000);
    r.desc(0x3030, cmd(STOP), 0, 0);
    r.out_le32(r.chan(kRx), (WAKE << 16) | WAKE);
    r.sched.advance_to(15 * kMs);
    CHECK_EQ(r.eth.frames_received(), 3u);
    // Multicast with a hash bit set, our own frames, promiscuous mode.
    r.w16(MacioBmac::BHASH0 - 0x10, 0x0040);
    r.wire.incoming.clear();
    r.wire.incoming.push_back(frame(mc, other, 80, 1));
    r.wire.incoming.push_back(frame(other, kMe, 80, 2));  // not to us, from us: dropped
    r.desc(0x3030, cmd(INPUT_LAST, 3), kRxBuf, 0x36000);
    r.desc(0x3040, cmd(INPUT_LAST, 3), kRxBuf, 0x38000);
    r.desc(0x3050, cmd(STOP), 0, 0);
    r.out_le32(r.chan(kRx), (WAKE << 16) | WAKE);
    r.sched.advance_to(25 * kMs);
    CHECK_EQ(r.eth.frames_received(), 5u);  // the fourth waiting frame, then the multicast one
    r.w16(MacioBmac::RXCFG, 0x0100 | 0x0040 | 1);  // promiscuous
    r.wire.incoming.push_back(frame(other, other, 80, 3));
    r.desc(0x3050, cmd(INPUT_LAST, 3), kRxBuf, 0x3A000);
    r.desc(0x3060, cmd(STOP), 0, 0);
    r.out_le32(r.chan(kRx), (WAKE << 16) | WAKE);
    r.sched.advance_to(35 * kMs);
    CHECK_EQ(r.eth.frames_received(), 6u);
    // Receiver off: frames are dropped and polling stops.
    r.w16(MacioBmac::RXCFG, 0);
    r.sched.advance_to(40 * kMs);
    CHECK_EQ(r.sched.pending_count(), 0u);
}
