// SPDX-License-Identifier: GPL-3.0-or-later
// The mac-io IDE cell + DBDMA, driven through the bus like Linux pata_macio:
// taskfile registers at n * 0x10, control at 0x160, DMA via a descriptor
// list (INPUT/OUTPUT_MORE..LAST + STOP), cmdptr -> command -> RUN.

#include "core/common/endian.h"
#include "core/devices/macio/dbdma.h"
#include "core/devices/macio/macio_ide.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;
using namespace g3::dbdma;

namespace {

constexpr u32 kIde = 0x80820000u;
constexpr u32 kDma = 0x80808000u;
constexpr u32 kCh = 0x16;
constexpr u32 kChRegs = kDma + kCh * DbdmaController::kChannelStride;

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x40000);
    std::vector<u8> image = std::vector<u8>(128 * 512);
    RamBlockDevice disk{image.data(), image.size()};
    PhysicalBus bus;
    DbdmaController dma{bus};
    MacioIde ide;
    bool intrq = false;
    int intrq_edges = 0;
    int dma_irqs = 0;
    Rig()
    {
        for (size_t i = 0; i < image.size(); ++i)
            image[i] = static_cast<u8>(i * 13 + (i >> 9));
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        bus.map_mmio(kDma, DbdmaController::kWindowSize, &dma);
        bus.map_mmio(kIde, MacioIde::kWindowSize, &ide);
        ide.attach_dma(&dma.channel(kCh));
        ide.bus().attach(0, &disk);
        ide.set_irq([](void* c, bool a) {
            auto* r = static_cast<Rig*>(c);
            if (a && !r->intrq)
                ++r->intrq_edges;
            r->intrq = a;
        }, this);
        dma.channel(kCh).set_irq([](void* c) { ++*static_cast<int*>(c); }, &dma_irqs);
    }
    // Byte-wide taskfile access.
    void outb(u32 reg, u8 v) { bus.write8(kIde + reg * 0x10, v); }
    u8 inb(u32 reg)
    {
        u8 v = 0;
        bus.read8(kIde + reg * 0x10, v);
        return v;
    }
    u8 altstatus()
    {
        u8 v = 0;
        bus.read8(kIde + 0x160, v);
        return v;
    }
    void out_le32(u32 a, u32 v) { bus.write32(a, byteswap32(v)); }
    u32 in_le32(u32 a)
    {
        u32 v = 0;
        bus.read32(a, v);
        return byteswap32(v);
    }
    void desc(u32 at, u16 cmd, u16 req, u32 addr)
    {
        store_le16(&ram[at + 0], req);
        store_le16(&ram[at + 2], cmd);
        store_le32(&ram[at + 4], addr);
        store_le32(&ram[at + 8], 0);
        store_le32(&ram[at + 12], 0);
    }
    void taskfile_lba(u32 lba, u8 count)
    {
        outb(2, count);
        outb(3, static_cast<u8>(lba));
        outb(4, static_cast<u8>(lba >> 8));
        outb(5, static_cast<u8>(lba >> 16));
        outb(6, static_cast<u8>(0xE0 | ((lba >> 24) & 0xF)));
    }
};

} // namespace

TEST_CASE("devices/macio_ide/pio_identify_byte_order")
{
    Rig r;
    r.outb(6, 0xA0);
    r.outb(7, 0xEC);
    CHECK(r.intrq);
    CHECK(r.altstatus() & 0x08);
    // ioread16_rep: halfword reads stored as they come; the IDENTIFY data
    // must appear in memory as little-endian words (libata swaps them).
    std::vector<u8> mem(512);
    for (u32 i = 0; i < 512; i += 2) {
        u16 v = 0;
        r.bus.read16(kIde, v);
        mem[i] = static_cast<u8>(v >> 8);       // big-endian store of the halfword
        mem[i + 1] = static_cast<u8>(v);
    }
    const u16 w27 = static_cast<u16>(mem[54] | (mem[55] << 8));
    CHECK_EQ(w27, static_cast<u16>(('G' << 8) | '3'));
    const u32 cap = static_cast<u32>(mem[120] | (mem[121] << 8) | (mem[122] << 16) | (mem[123] << 24));
    CHECK_EQ(cap, 128u);
    CHECK_EQ(r.inb(7) & 0x08, 0u);               // DRQ gone; status read clears INTRQ
    CHECK(!r.intrq);
}

TEST_CASE("devices/macio_ide/pio_sector_32bit_data")
{
    Rig r;
    r.taskfile_lba(7, 1);
    r.outb(7, 0x20);
    r.inb(7);
    u32 w = 0;
    r.bus.read32(kIde, w);                        // 32-bit data access: 4 stream bytes
    CHECK_EQ(w >> 24, r.image[7 * 512 + 0]);
    CHECK_EQ(w & 0xFF, r.image[7 * 512 + 3]);
    // PIO write with 32-bit data accesses keeps the stream order too.
    r.taskfile_lba(8, 1);
    r.outb(7, 0x30);
    for (u32 i = 0; i < 512; i += 4)
        r.bus.write32(kIde, 0x01020304u + i);
    CHECK_EQ(r.image[8 * 512 + 0], 0x01u);
    CHECK_EQ(r.image[8 * 512 + 3], 0x04u);
    CHECK(r.intrq);
}

TEST_CASE("devices/macio_ide/linux_dma_read_and_write")
{
    Rig r;
    // Read 5 sectors into two scatter-gather segments: 0x600 bytes at
    // 0x10000 and 0x400 bytes at 0x20000.
    r.desc(0x1000, INPUT_MORE << 12, 0x600, 0x10000);
    r.desc(0x1010, INPUT_LAST << 12, 0x400, 0x20000);
    r.desc(0x1020, STOP << 12, 0, 0);
    r.out_le32(kChRegs + 0x0C, 0x1000);           // bmdma_setup: cmdptr
    r.taskfile_lba(30, 5);
    r.outb(7, 0xC8);                              // READ DMA
    CHECK(!r.intrq);                              // waits for the DMA engine
    r.out_le32(kChRegs, (RUN << 16) | RUN);       // bmdma_start
    CHECK(r.intrq);                               // disk interrupt at completion
    const u32 st = r.in_le32(kChRegs + 0x04);     // bmdma_status
    CHECK_EQ(st & (RUN | DEAD), RUN);
    CHECK_EQ(st & ACTIVE, 0u);
    CHECK_EQ(r.ram[0x10000], r.image[30 * 512]);
    CHECK_EQ(r.ram[0x105FF], r.image[30 * 512 + 0x5FF]);
    CHECK_EQ(r.ram[0x20000], r.image[30 * 512 + 0x600]);
    CHECK_EQ(r.ram[0x203FF], r.image[35 * 512 - 1]);
    CHECK_EQ(r.inb(7) & 0x89, 0x00u);             // !BSY !DRQ !ERR
    // bmdma_stop: (RUN|WAKE|DEAD) << 16, wait for !RUN.
    r.out_le32(kChRegs, (RUN | WAKE | DEAD) << 16);
    CHECK_EQ(r.in_le32(kChRegs + 0x04) & RUN, 0u);

    // Write 2 sectors from one segment, RUN set before the command this time.
    for (u32 i = 0; i < 1024; ++i)
        r.ram[0x30000 + i] = static_cast<u8>(0xA5 ^ i);
    r.desc(0x1000, OUTPUT_LAST << 12, 1024, 0x30000);
    r.desc(0x1010, STOP << 12, 0, 0);
    r.out_le32(kChRegs + 0x0C, 0x1000);
    r.out_le32(kChRegs, (RUN << 16) | RUN);       // device not ready yet: channel waits
    CHECK(r.in_le32(kChRegs + 0x04) & ACTIVE);
    r.taskfile_lba(90, 2);
    r.outb(7, 0xCA);                              // WRITE DMA: the request kicks the channel
    CHECK(r.intrq);
    CHECK_EQ(r.in_le32(kChRegs + 0x04) & ACTIVE, 0u);
    CHECK_EQ(r.image[90 * 512 + 0], 0xA5u);
    CHECK_EQ(r.image[91 * 512 + 511], static_cast<u8>(0xA5 ^ 1023));
    CHECK_EQ(r.dma_irqs, 0);                      // Linux sets no descriptor interrupts
}

TEST_CASE("devices/macio_ide/control_and_timing_registers")
{
    Rig r;
    r.bus.write8(kIde + 0x160, 0x02);             // nIEN
    r.outb(6, 0xA0);
    r.outb(7, 0xE5);
    CHECK(!r.intrq);
    r.bus.write8(kIde + 0x160, 0x00);
    CHECK(r.intrq);
    r.bus.write32(kIde + 0x200, byteswap32(0x12345678u));  // writel (little-endian)
    CHECK_EQ(r.ide.timing(), 0x12345678u);
    CHECK_EQ(r.in_le32(kIde + 0x200), 0x12345678u);
    u16 h = 0;
    r.bus.read16(kIde + 0x70, h);                 // wide access to a byte register
    CHECK_EQ(h, 0xFFFFu);
}
