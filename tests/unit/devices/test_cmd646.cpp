// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/devices/ata/cmd646: PCI configuration, I/O windows in PCI
// I/O space, PIO through the taskfile, interrupts and bus-master DMA.

#include "core/common/endian.h"
#include "core/devices/ata/cmd646.h"
#include "core/storage/block_device.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;
using namespace g3::ata;

namespace {

// I/O addresses as the B&W's firmware assigned them (Linux boot log:
// cmd 0x1440, ctl 0x1430, bmdma 0x1400).
constexpr u32 kCmd = 0x1440, kCtl = 0x1430, kCmd2 = 0x1448, kCtl2 = 0x1434, kBm = 0x1400;
constexpr u32 kIo = Cmd646::kPciIoBase;

struct Rig {
    std::vector<u8> ram = std::vector<u8>(1u << 20);
    PhysicalBus bus;
    Cmd646 ide{bus};
    std::vector<u8> disk_bytes = std::vector<u8>(256 * 512);
    RamBlockDevice disk{disk_bytes.data(), disk_bytes.size()};
    int irq_edges = 0;
    bool irq = false;
    Rig()
    {
        bus.map_ram(0, ram.data(), static_cast<u32>(ram.size()));
        for (size_t i = 0; i < disk_bytes.size(); ++i)
            disk_bytes[i] = static_cast<u8>(i / 512 + i % 199);
        ide.channel(0).attach(0, &disk);
        ide.set_irq([](void* c, bool a) {
            auto* r = static_cast<Rig*>(c);
            if (a && !r->irq) ++r->irq_edges;
            r->irq = a;
        }, this);
    }
    void cfg32(u32 reg, u32 v) { ide.config_write(reg, v, 0xF); }
    void assign_and_enable()
    {
        cfg32(0x10, kCmd);
        cfg32(0x14, kCtl);
        cfg32(0x18, kCmd2);
        cfg32(0x1C, kCtl2);
        cfg32(0x20, kBm);
        cfg32(0x04, 0x0005);  // I/O space + bus master
    }
    u8 in8(u32 a) { u8 v = 0; bus.read8(kIo + a, v); return v; }
    void out8(u32 a, u8 v) { bus.write8(kIo + a, v); }
    u16 in16(u32 a) { u16 v = 0; bus.read16(kIo + a, v); return v; }
    void out32le(u32 a, u32 v) { bus.write32(kIo + a, byteswap32(v)); }  // what stwbrx does
};

} // namespace

TEST_CASE("devices/cmd646/config_space")
{
    Rig r;
    CHECK_EQ(r.ide.config_read(0x00), 0x06461095u);
    CHECK_EQ(r.ide.config_read(0x08), 0x01018F07u);           // class 01 01 8F, revision 7
    CHECK_EQ((r.ide.config_read(0x3C) >> 8) & 0xFF, 1u);       // interrupt pin A
    // BAR sizing (write all ones): I/O BARs of 8, 4, 8, 4 and 16 bytes.
    const u32 sizes[5] = {8, 4, 8, 4, 16};
    for (u32 i = 0; i < 5; ++i) {
        r.cfg32(0x10 + 4 * i, 0xFFFFFFFFu);
        const u32 v = r.ide.config_read(0x10 + 4 * i);
        CHECK_EQ(v & 1u, 1u);                                  // I/O space
        CHECK_EQ(~(v & ~3u) + 1, sizes[i]);
    }
    // Device-specific registers are stored.
    r.ide.config_write(0x50, 0x00004400, 0x2);
    CHECK_EQ((r.ide.config_read(0x50) >> 8) & 0xFF, 0x44u);
}

TEST_CASE("devices/cmd646/io_windows_follow_bars_and_command")
{
    Rig r;
    u8 v = 0;
    CHECK(!r.bus.read8(kIo + kCmd + 7, v));                    // nothing mapped yet
    r.assign_and_enable();
    CHECK_EQ(r.ide.io_base(0), kIo + kCmd);
    CHECK_EQ(r.ide.io_base(4), kIo + kBm);
    CHECK_EQ(r.in8(kCmd + 7), DRDY | DSC);                     // status of the attached disk
    CHECK_EQ(r.in8(kCtl + 2), DRDY | DSC);                     // alternate status
    CHECK_EQ(r.in8(kCmd2 + 7), 0x7Fu);                         // empty secondary channel
    r.cfg32(0x04, 0);                                          // I/O decoding off
    CHECK(!r.bus.read8(kIo + kCmd + 7, v));
    r.cfg32(0x04, 1);
    r.cfg32(0x10, 0x1500);                                     // moved
    CHECK(!r.bus.read8(kIo + kCmd + 7, v));
    CHECK_EQ(r.in8(0x1500 + 7), DRDY | DSC);
}

TEST_CASE("devices/cmd646/pio_identify_and_interrupt")
{
    Rig r;
    r.assign_and_enable();
    r.out8(kCmd + 6, 0xA0);
    r.out8(kCmd + 7, 0xEC);                                    // IDENTIFY DEVICE
    CHECK(r.irq);
    CHECK_EQ(r.irq_edges, 1);
    CHECK_EQ(r.in8(kBm + 1) & 0x04, 0x04u);                    // MRDMODE: channel 0 interrupt
    CHECK_EQ((r.ide.config_read(0x70) >> 8) & 0x04, 0x04u);    // also in config space
    CHECK_EQ(r.in8(kBm + 2) & 0x04, 0x04u);                    // bus master status: interrupt
    CHECK_EQ(r.in8(kCmd + 7) & DRQ, DRQ);                      // status read: INTRQ off
    CHECK(!r.irq);
    // Word 27: model string; the first character is the word's high byte,
    // which comes second in the byte stream; a 16-bit read returns the
    // stream's first byte in its high half.
    std::vector<u16> w(256);
    for (u32 i = 0; i < 256; ++i)
        w[i] = r.in16(kCmd);
    CHECK_EQ(w[0], 0x4000u);                                   // stream: 0x40 0x00 -> word 0x0040
    CHECK_EQ(w[27], static_cast<u16>(('3' << 8) | 'G'));
    CHECK_EQ(r.in8(kCmd + 7) & DRQ, 0u);
}

TEST_CASE("devices/cmd646/mrdmode_blocks_and_clears")
{
    Rig r;
    r.assign_and_enable();
    r.out8(kBm + 1, 0x10);                                     // block channel 0
    r.out8(kCmd + 6, 0xA0);
    r.out8(kCmd + 7, 0xE5);                                    // CHECK POWER MODE: interrupt
    CHECK(!r.irq);                                             // blocked
    CHECK_EQ(r.in8(kBm + 1) & 0x04, 0x04u);                    // but flagged
    r.out8(kBm + 1, 0x00);                                     // unblock
    CHECK(r.irq);
    r.out8(kBm + 1, 0x04);                                     // write 1 to clear the flag
    CHECK_EQ(r.in8(kBm + 1) & 0x04, 0u);
    r.in8(kCmd + 7);                                           // the drive drops INTRQ
    CHECK(!r.irq);
    r.out8(kBm + 2, 0x04);                                     // bus master interrupt bit: W1C
    CHECK_EQ(r.in8(kBm + 2) & 0x04, 0u);
    r.out8(kBm + 2, 0x60);                                     // drive DMA capable bits: R/W
    CHECK_EQ(r.in8(kBm + 2) & 0x60, 0x60u);
}

TEST_CASE("devices/cmd646/bus_master_dma_read")
{
    Rig r;
    r.assign_and_enable();
    // PRD table at 0x1000: 1024 bytes to 0x8000, then 512 bytes to 0x9000 (end).
    auto le32 = [&](u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) r.ram[a + k] = static_cast<u8>(v >> (8 * k)); };
    le32(0x1000, 0x8000);
    le32(0x1004, 1024);
    le32(0x1008, 0x9000);
    le32(0x100C, 512 | 0x80000000u);
    r.out32le(kBm + 4, 0x1000);
    CHECK_EQ(r.in8(kBm + 4), 0x00u);
    CHECK_EQ(r.in8(kBm + 5), 0x10u);
    r.out8(kBm + 0, 0x08);                                     // direction: to memory
    r.out8(kCmd + 2, 3);                                       // 3 sectors from LBA 10
    r.out8(kCmd + 3, 10);
    r.out8(kCmd + 4, 0);
    r.out8(kCmd + 5, 0);
    r.out8(kCmd + 6, 0xE0);
    r.out8(kCmd + 7, 0xC8);                                    // READ DMA
    CHECK(!r.irq);                                             // nothing until the engine runs
    r.out8(kBm + 0, 0x09);                                     // start
    CHECK_EQ(r.ram[0x8000], r.disk_bytes[10 * 512]);
    CHECK_EQ(r.ram[0x8000 + 1023], r.disk_bytes[10 * 512 + 1023]);
    CHECK_EQ(r.ram[0x9000 + 511], r.disk_bytes[12 * 512 + 511]);
    const u8 st = r.in8(kBm + 2);
    CHECK_EQ(st & 0x01, 0u);                                   // not active: table done
    CHECK_EQ(st & 0x04, 0x04u);                                // interrupt
    CHECK_EQ(st & 0x02, 0u);
    CHECK(r.irq);
    CHECK_EQ(r.in8(kCmd + 7) & (DRQ | ERR | BSY), 0u);
    r.out8(kBm + 0, 0x00);                                     // stop
}

TEST_CASE("devices/cmd646/bus_master_dma_write_and_bad_table")
{
    Rig r;
    r.assign_and_enable();
    auto le32 = [&](u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) r.ram[a + k] = static_cast<u8>(v >> (8 * k)); };
    for (u32 i = 0; i < 512; ++i)
        r.ram[0xA000 + i] = static_cast<u8>(0x5A ^ i);
    le32(0x2000, 0xA000);
    le32(0x2004, 512 | 0x80000000u);
    r.out32le(kBm + 4, 0x2000);
    r.out8(kCmd + 2, 1);
    r.out8(kCmd + 3, 7);
    r.out8(kCmd + 4, 0);
    r.out8(kCmd + 5, 0);
    r.out8(kCmd + 6, 0xE0);
    r.out8(kCmd + 7, 0xCA);                                    // WRITE DMA
    r.out8(kBm + 0, 0x01);                                     // start, memory to drive
    CHECK_EQ(r.disk_bytes[7 * 512 + 3], static_cast<u8>(0x5A ^ 3));
    CHECK_EQ(r.in8(kBm + 2) & 0x05, 0x04u);
    r.out8(kBm + 0, 0x00);
    // A table pointing outside RAM: error, engine stops.
    le32(0x2000, 0xF0000000u);
    r.out8(kCmd + 7, 0xCA);
    r.out8(kBm + 0, 0x01);
    CHECK_EQ(r.in8(kBm + 2) & 0x03, 0x02u);                    // error, not active
}
