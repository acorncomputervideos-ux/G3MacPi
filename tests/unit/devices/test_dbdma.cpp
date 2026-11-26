// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the DBDMA controller, driven through the bus the way a driver
// does it (little-endian descriptors in RAM, out_le32 register writes).

#include "core/common/endian.h"
#include "core/devices/macio/dbdma.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;
using namespace g3::dbdma;

namespace {

constexpr u32 kBase = 0x80808000u;   // controller (mac-io BAR + 0x8000)
constexpr u32 kCh = 3;               // test channel
constexpr u32 kRegs = kBase + kCh * DbdmaController::kChannelStride;

struct Device final : DbdmaStream {
    std::vector<u8> sink;            // received by output commands
    std::vector<u8> source;          // supplied to input commands
    size_t pos = 0;
    u32 accept_limit = 0xFFFFFFFF;   // bytes the device accepts before "not ready"
    bool saw_last = false;
    u32 dma_output(const u8* d, u32 len, bool last) override
    {
        const u32 n = len < accept_limit ? len : accept_limit;
        sink.insert(sink.end(), d, d + n);
        accept_limit -= n;
        if (n == len && last)
            saw_last = true;
        return n;
    }
    u32 dma_input(u8* d, u32 len) override
    {
        u32 n = 0;
        while (n < len && pos < source.size())
            d[n++] = source[pos++];
        return n;
    }
};

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x10000);
    PhysicalBus bus;
    DbdmaController dma{bus};
    Device dev;
    int irqs = 0;
    Rig()
    {
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        bus.map_mmio(kBase, DbdmaController::kWindowSize, &dma);
        dma.channel(kCh).attach_stream(&dev);
        dma.channel(kCh).set_irq([](void* c) { ++*static_cast<int*>(c); }, &irqs);
    }
    void out_le32(u32 a, u32 v) { bus.write32(a, byteswap32(v)); }
    u32 in_le32(u32 a)
    {
        u32 v = 0;
        bus.read32(a, v);
        return byteswap32(v);
    }
    // Descriptor helper (all fields little-endian, like Linux's st_le16/32).
    void desc(u32 at, u16 cmd, u16 req, u32 addr, u32 dep = 0)
    {
        store_le16(&ram[at + 0], req);
        store_le16(&ram[at + 2], cmd);
        store_le32(&ram[at + 4], addr);
        store_le32(&ram[at + 8], dep);
        store_le16(&ram[at + 12], 0xAAAA);
        store_le16(&ram[at + 14], 0xAAAA);
    }
    u16 res_count(u32 at) { return load_le16(&ram[at + 12]); }
    u16 xfer_status(u32 at) { return load_le16(&ram[at + 14]); }
    void start(u32 cmdptr)
    {
        out_le32(kRegs + 0x0C, cmdptr);
        out_le32(kRegs + 0x00, (RUN << 16) | RUN);
    }
    u32 status() { return in_le32(kRegs + 0x04); }
};

constexpr u16 cmd(u16 c, u16 key = 0, u16 i = 0, u16 b = 0, u16 w = 0)
{
    return static_cast<u16>((c << 12) | (key << 8) | (i << 4) | (b << 2) | w);
}

} // namespace

TEST_CASE("devices/dbdma/output_then_stop")
{
    Rig r;
    for (u32 i = 0; i < 8; ++i)
        r.ram[0x2000 + i] = static_cast<u8>(0x10 + i);
    r.desc(0x1000, cmd(OUTPUT_MORE), 3, 0x2000);
    r.desc(0x1010, cmd(OUTPUT_LAST, 0, 3 /*INTR_ALWAYS*/), 5, 0x2003);
    r.desc(0x1020, cmd(STOP), 0, 0);
    r.start(0x1000);
    REQUIRE(r.dev.sink.size() == 8);
    CHECK_EQ(r.dev.sink[0], 0x10u);
    CHECK_EQ(r.dev.sink[7], 0x17u);
    CHECK(r.dev.saw_last);
    CHECK_EQ(r.irqs, 1);                         // only the OUTPUT_LAST interrupts
    CHECK_EQ(r.res_count(0x1000), 0u);           // residual written back
    CHECK_EQ(r.xfer_status(0x1000), RUN | ACTIVE);   // status written back
    CHECK_EQ(r.xfer_status(0x1010), RUN | ACTIVE);
    CHECK_EQ(r.xfer_status(0x1020), 0xAAAAu);        // STOP writes nothing
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1020u);  // parked on the STOP
    CHECK_EQ(r.status() & ACTIVE, 0u);
    CHECK(r.status() & RUN);
}

namespace {
// Records the RAM writes a bus reports (the block cache's view).
struct WriteRecord {
    u32 count = 0, lo = 0xFFFFFFFFu, hi = 0;
    u32 min = 0;  // writes below this are not recorded
    static void hear(void* ctx, u32 pa, u32 len)
    {
        auto* w = static_cast<WriteRecord*>(ctx);
        if (pa < w->min)
            return;
        ++w->count;
        if (pa < w->lo) w->lo = pa;
        if (pa + len > w->hi) w->hi = pa + len;
    }
};
} // namespace

TEST_CASE("devices/dbdma/input_and_append_with_wake")
{
    Rig r;
    r.dev.source = {1, 2, 3, 4, 5, 6};
    r.desc(0x1000, cmd(INPUT_LAST), 4, 0x3000);
    r.desc(0x1010, cmd(STOP), 0, 0);
    WriteRecord heard;
    heard.min = 0x2000;                                        // not the descriptors' status words
    r.bus.set_write_observer(WriteRecord::hear, &heard);
    r.start(0x1000);
    CHECK_EQ(heard.lo, 0x3000u);                               // input DMA reports what it wrote
    CHECK_EQ(heard.hi, 0x3004u);
    r.bus.set_write_observer(nullptr, nullptr);
    CHECK_EQ(r.ram[0x3000], 1u);
    CHECK_EQ(r.ram[0x3003], 4u);
    // Driver pattern: replace the STOP with new work, add a new STOP, WAKE.
    r.desc(0x1010, cmd(INPUT_LAST), 2, 0x3010);
    r.desc(0x1020, cmd(STOP), 0, 0);
    r.out_le32(kRegs, (WAKE << 16) | WAKE);
    CHECK_EQ(r.ram[0x3010], 5u);
    CHECK_EQ(r.ram[0x3011], 6u);
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1020u);
}

TEST_CASE("devices/dbdma/branch_and_interrupt_conditions")
{
    Rig r;
    // BR_IFSET on device-status bit 0: br_sel mask 1, value 1.
    r.out_le32(kRegs + 0x14, 0x00010001u);
    r.out_le32(kRegs + 0x10, 0x00020002u);       // intr cond: bit 1 set
    r.desc(0x1000, cmd(NOP, 0, 1 /*IFSET*/, 1 /*BR_IFSET*/), 0, 0, 0x1100);
    r.desc(0x1010, cmd(STOP), 0, 0);             // fall-through target
    r.desc(0x1100, cmd(STOP), 0, 0);             // branch target
    // Device status bit 0 set, bit 1 clear: branch taken, no interrupt.
    r.dma.channel(kCh).set_device_status(0x01);
    r.start(0x1000);
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1100u);
    CHECK(r.status() & BT);
    CHECK_EQ(r.irqs, 0);
    // Again with both bits set: interrupt this time.
    r.out_le32(kRegs, RUN << 16);                // stop
    r.dma.channel(kCh).set_device_status(0x03);
    r.start(0x1000);
    CHECK_EQ(r.irqs, 1);
    // With bit 0 clear: no branch.
    r.out_le32(kRegs, RUN << 16);
    r.dma.channel(kCh).set_device_status(0x00);
    r.start(0x1000);
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1010u);
    CHECK_EQ(r.status() & BT, 0u);
}

TEST_CASE("devices/dbdma/wait_condition_and_not_ready_device")
{
    Rig r;
    r.out_le32(kRegs + 0x18, 0x00040004u);       // wait while device bit 2 set
    r.desc(0x1000, cmd(NOP, 0, 0, 0, 1 /*WAIT_IFSET*/), 0, 0);
    r.desc(0x1010, cmd(OUTPUT_LAST), 4, 0x2000);
    r.desc(0x1020, cmd(STOP), 0, 0);
    r.dma.channel(kCh).set_device_status(0x04);
    r.dev.accept_limit = 2;                       // device takes only 2 bytes for now
    r.start(0x1000);
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1000u);   // waiting
    CHECK(r.status() & ACTIVE);
    r.dma.channel(kCh).set_device_status(0x00);   // condition gone: continues
    CHECK_EQ(r.dev.sink.size(), 2u);              // stalled mid-transfer
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1010u);
    r.dev.accept_limit = 100;
    r.dma.channel(kCh).kick();                    // device ready again
    CHECK_EQ(r.dev.sink.size(), 4u);
    CHECK_EQ(r.in_le32(kRegs + 0x0C), 0x1020u);
}

TEST_CASE("devices/dbdma/linux_stop_and_reset_macros")
{
    Rig r;
    r.desc(0x1000, cmd(NOP, 0, 0, 3 /*BR_ALWAYS*/), 0, 0, 0x1000);  // loops forever
    r.start(0x1000);
    // DBDMA_DO_STOP: write (RUN|FLUSH)<<16, then poll until !(ACTIVE|FLUSH).
    r.out_le32(kRegs, (RUN | FLUSH) << 16);
    CHECK_EQ(r.status() & (ACTIVE | FLUSH), 0u);
    // A FLUSH request on a running channel is acknowledged (bit reads 0).
    r.start(0x1000);
    r.out_le32(kRegs, (FLUSH << 16) | FLUSH);
    CHECK_EQ(r.status() & FLUSH, 0u);
    CHECK(r.status() & RUN);
    r.out_le32(kRegs, RUN << 16);
    // DBDMA_DO_RESET: mask of everything, then poll until !RUN.
    r.start(0x1000);
    r.out_le32(kRegs, (ACTIVE | DEAD | WAKE | FLUSH | PAUSE | RUN) << 16);
    CHECK_EQ(r.status() & RUN, 0u);
}

TEST_CASE("devices/dbdma/dead_and_load_store_word")
{
    Rig r;
    r.desc(0x1000, cmd(OUTPUT_MORE), 4, 0);       // null address -> dead
    r.start(0x1000);
    CHECK(r.status() & DEAD);
    CHECK_EQ(r.status() & ACTIVE, 0u);
    CHECK_EQ(r.irqs, 1);
    r.out_le32(kRegs, RUN << 16);                 // clearing RUN clears DEAD
    CHECK_EQ(r.status() & DEAD, 0u);

    // STORE_WORD: 4 bytes of cmd_dep to memory; LOAD_WORD: memory -> cmd_dep.
    r.desc(0x1000, cmd(STORE_WORD, KEY_SYSTEM), 4, 0x4000, 0xDDCCBBAAu);
    r.desc(0x1010, cmd(LOAD_WORD, KEY_SYSTEM), 4, 0x4000, 0);
    r.desc(0x1020, cmd(STOP), 0, 0);
    r.start(0x1000);
    CHECK_EQ(r.ram[0x4000], 0xAAu);
    CHECK_EQ(r.ram[0x4003], 0xDDu);
    CHECK_EQ(load_le32(&r.ram[0x1018]), 0xDDCCBBAAu);
}
