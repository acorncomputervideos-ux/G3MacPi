// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the ATA channel model (protocol per ATA/ATAPI-5/6).

#include "core/devices/ata/ata_bus.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;
using namespace g3::ata;

namespace {

struct Disk {
    std::vector<u8> bytes;
    RamBlockDevice dev;
    explicit Disk(u32 sectors, bool ro = false)
        : bytes(static_cast<size_t>(sectors) * 512), dev(bytes.data(), bytes.size(), ro)
    {
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<u8>((i / 512) * 3 + i % 251);
    }
    u8 expected(u64 lba, u32 off) const { return bytes[lba * 512 + off]; }
};

struct Rig {
    AtaBus bus;
    int edges = 0;
    Rig() { bus.set_irq([](void* c, bool a) { if (a) ++*static_cast<int*>(c); }, &edges); }
    void lba(u32 l, u8 count, u32 dev = 0)
    {
        bus.write_reg(kSectorCount, count);
        bus.write_reg(kLbaLow, static_cast<u8>(l));
        bus.write_reg(kLbaMid, static_cast<u8>(l >> 8));
        bus.write_reg(kLbaHigh, static_cast<u8>(l >> 16));
        bus.write_reg(kDevice, static_cast<u8>(0xE0 | (dev ? DEV_SLAVE : 0) | ((l >> 24) & 0xF)));
    }
    std::vector<u8> pio_in(u32 n)
    {
        std::vector<u8> v(n);
        for (u32 i = 0; i < n; i += 2)
            bus.read_data(&v[i], 2);
        return v;
    }
};

u16 id_word(const std::vector<u8>& id, u32 w) { return static_cast<u16>(id[2 * w] | (id[2 * w + 1] << 8)); }

} // namespace

TEST_CASE("devices/ata/signature_and_absent_devices")
{
    Rig r;
    CHECK_EQ(r.bus.read_reg(kStatus), 0x7Fu);    // empty bus: DD7 pulled low
    Disk d(100);
    r.bus.attach(0, &d.dev);
    CHECK_EQ(r.bus.read_reg(kSectorCount), 1u);  // ATA signature
    CHECK_EQ(r.bus.read_reg(kLbaLow), 1u);
    CHECK_EQ(r.bus.read_reg(kLbaMid), 0u);
    CHECK_EQ(r.bus.read_reg(kLbaHigh), 0u);
    CHECK_EQ(r.bus.read_reg(kStatus), static_cast<u32>(DRDY | DSC));
    // Select the absent device 1: device 0 answers status reads with 0.
    r.bus.write_reg(kDevice, 0xB0);
    CHECK_EQ(r.bus.read_reg(kStatus), 0u);
    CHECK_EQ(r.bus.read_alt_status(), 0u);
    r.bus.write_reg(kCommand, 0xEC);              // ignored: nobody there
    CHECK_EQ(r.edges, 0);
    // A pending interrupt of device 0 is driven only while it is selected.
    r.bus.write_reg(kDevice, 0xA0);
    r.bus.write_reg(kCommand, 0xE5);
    CHECK(r.bus.irq_asserted());
    r.bus.write_reg(kDevice, 0xB0);
    CHECK(!r.bus.irq_asserted());
    r.bus.write_reg(kDevice, 0xA0);
    CHECK(r.bus.irq_asserted());
    r.bus.read_reg(kStatus);
    r.bus.write_reg(kDevice, 0xB0);
    // Registers are shared: device 0 sees what was written while 1 was selected.
    r.bus.write_reg(kLbaMid, 0x5A);
    r.bus.write_reg(kDevice, 0xA0);
    CHECK_EQ(r.bus.read_reg(kLbaMid), 0x5Au);
}

TEST_CASE("devices/ata/identify_device")
{
    Rig r;
    Disk d(20000);
    r.bus.attach(0, &d.dev);
    r.bus.write_reg(kDevice, 0xA0);
    r.bus.write_reg(kCommand, 0xEC);
    CHECK_EQ(r.edges, 1);
    CHECK(r.bus.read_alt_status() & DRQ);
    const std::vector<u8> id = r.pio_in(512);
    CHECK_EQ(r.bus.read_reg(kStatus) & (DRQ | BSY | ERR), 0u);
    CHECK_EQ(id_word(id, 0) & 0x8000, 0u);           // ATA device
    CHECK(id_word(id, 49) & 0x0200);                  // LBA supported
    CHECK(id_word(id, 49) & 0x0100);                  // DMA supported
    CHECK_EQ(id_word(id, 60) | (id_word(id, 61) << 16), 20000u);
    CHECK_EQ(id_word(id, 3), 16u);
    CHECK_EQ(id_word(id, 6), 63u);
    CHECK_EQ(id_word(id, 1), 20000u / (16 * 63));
    // Strings: first character in the high byte of each word.
    CHECK_EQ(id_word(id, 27), static_cast<u16>(('G' << 8) | '3'));
    CHECK_EQ(id_word(id, 46), 0x2020u);                 // padded with spaces
    CHECK_EQ(id_word(id, 47) & 0xFF, AtaBus::kMaxMultiple);
}

TEST_CASE("devices/ata/pio_read_sectors_interrupt_per_block")
{
    Rig r;
    Disk d(64);
    r.bus.attach(0, &d.dev);
    r.lba(10, 3);
    r.bus.write_reg(kCommand, 0x20);
    for (u32 s = 0; s < 3; ++s) {
        CHECK_EQ(r.edges, static_cast<int>(s + 1));  // one interrupt per block
        CHECK_EQ(r.bus.read_reg(kStatus) & (DRQ | ERR), static_cast<u32>(DRQ));  // clears INTRQ
        CHECK(!r.bus.irq_asserted());
        const std::vector<u8> sec = r.pio_in(512);
        CHECK_EQ(sec[0], d.expected(10 + s, 0));
        CHECK_EQ(sec[511], d.expected(10 + s, 511));
    }
    CHECK_EQ(r.edges, 3);                              // none after the last block
    CHECK_EQ(r.bus.read_reg(kStatus), static_cast<u32>(DRDY | DSC));
}

TEST_CASE("devices/ata/pio_write_sectors")
{
    Rig r;
    Disk d(64);
    r.bus.attach(0, &d.dev);
    r.lba(5, 2);
    r.bus.write_reg(kCommand, 0x30);
    CHECK_EQ(r.edges, 0);                              // no interrupt before the first block
    CHECK(r.bus.read_alt_status() & DRQ);
    u8 blk[512];
    for (u32 s = 0; s < 2; ++s) {
        for (u32 i = 0; i < 512; ++i)
            blk[i] = static_cast<u8>(0xC0 + s + i);
        for (u32 i = 0; i < 512; i += 4)
            r.bus.write_data(&blk[i], 4);              // 32-bit accesses
        CHECK_EQ(r.edges, static_cast<int>(s + 1));    // after each block
        r.bus.read_reg(kStatus);
    }
    CHECK_EQ(d.bytes[5 * 512], 0xC0u);
    CHECK_EQ(d.bytes[6 * 512 + 511], static_cast<u8>(0xC1 + 511));
    CHECK_EQ(r.bus.read_alt_status(), static_cast<u32>(DRDY | DSC));
}

TEST_CASE("devices/ata/multiple_mode")
{
    Rig r;
    Disk d(256);
    r.bus.attach(0, &d.dev);
    r.lba(0, 8);
    r.bus.write_reg(kCommand, 0xC4);                   // READ MULTIPLE before SET MULTIPLE
    CHECK(r.bus.read_reg(kStatus) & ERR);
    CHECK_EQ(r.bus.read_reg(kError), static_cast<u32>(ABRT));
    r.bus.write_reg(kSectorCount, 3);                  // not a power of two
    r.bus.write_reg(kCommand, 0xC6);
    CHECK(r.bus.read_reg(kStatus) & ERR);
    r.bus.write_reg(kSectorCount, 4);
    r.bus.write_reg(kCommand, 0xC6);
    CHECK_EQ(r.bus.read_reg(kStatus) & ERR, 0u);
    const int before = r.edges;
    r.lba(100, 10);                                    // 10 sectors in blocks of 4, 4, 2
    r.bus.write_reg(kCommand, 0xC4);
    std::vector<u8> all;
    for (u32 b = 0; b < 3; ++b) {
        r.bus.read_reg(kStatus);
        const std::vector<u8> blk = r.pio_in(b < 2 ? 2048 : 1024);
        all.insert(all.end(), blk.begin(), blk.end());
    }
    CHECK_EQ(r.edges - before, 3);
    REQUIRE(all.size() == 10 * 512);
    CHECK_EQ(all[9 * 512 + 7], d.expected(109, 7));
    CHECK_EQ(r.bus.read_reg(kStatus) & DRQ, 0u);
}

TEST_CASE("devices/ata/chs_addressing_and_errors")
{
    Rig r;
    Disk d(1000);
    r.bus.attach(0, &d.dev);
    // INITIALIZE DEVICE PARAMETERS: 4 heads (device bits + 1), 17 sectors.
    r.bus.write_reg(kSectorCount, 17);
    r.bus.write_reg(kDevice, 0xA3);
    r.bus.write_reg(kCommand, 0x91);
    CHECK_EQ(r.bus.read_reg(kStatus) & ERR, 0u);
    // C/H/S = 2/1/5 -> LBA (2*4 + 1)*17 + 4 = 157.
    r.bus.write_reg(kSectorCount, 1);
    r.bus.write_reg(kLbaLow, 5);
    r.bus.write_reg(kLbaMid, 2);
    r.bus.write_reg(kLbaHigh, 0);
    r.bus.write_reg(kDevice, 0xA1);
    r.bus.write_reg(kCommand, 0x20);
    r.bus.read_reg(kStatus);
    CHECK_EQ(r.pio_in(512)[3], d.expected(157, 3));
    // Sector 0 is invalid in CHS.
    r.bus.write_reg(kLbaLow, 0);
    r.bus.write_reg(kCommand, 0x20);
    CHECK(r.bus.read_reg(kStatus) & ERR);
    CHECK_EQ(r.bus.read_reg(kError), static_cast<u32>(IDNF));
    // Beyond the end (LBA).
    r.lba(999, 2);
    r.bus.write_reg(kCommand, 0x20);
    CHECK_EQ(r.bus.read_reg(kError), static_cast<u32>(IDNF));
    // Unknown command; PACKET on an ATA device.
    r.bus.write_reg(kCommand, 0x5F);
    CHECK_EQ(r.bus.read_reg(kError), static_cast<u32>(ABRT));
    r.bus.write_reg(kCommand, 0xA0);
    CHECK_EQ(r.bus.read_reg(kError), static_cast<u32>(ABRT));
    // Sector count 0 = 256 sectors.
    r.lba(744, 0);
    r.bus.write_reg(kCommand, 0x40);                   // READ VERIFY: 744 + 256 = 1000 fits
    CHECK_EQ(r.bus.read_reg(kStatus) & ERR, 0u);
    r.lba(745, 0);
    r.bus.write_reg(kCommand, 0x40);
    CHECK(r.bus.read_reg(kStatus) & ERR);
}

TEST_CASE("devices/ata/nien_srst_and_diagnostic")
{
    Rig r;
    Disk d0(64), d1(64);
    r.bus.attach(0, &d0.dev);
    r.bus.attach(1, &d1.dev);
    r.bus.write_device_control(NIEN);
    r.bus.write_reg(kDevice, 0xA0);
    r.bus.write_reg(kCommand, 0xE5);                   // CHECK POWER MODE
    CHECK(!r.bus.irq_asserted());                      // masked
    CHECK_EQ(r.bus.read_reg(kSectorCount), 0xFFu);
    r.bus.write_device_control(0);                     // unmasking shows the pending interrupt
    CHECK(r.bus.irq_asserted());
    CHECK_EQ(r.bus.read_alt_status(), static_cast<u32>(DRDY | DSC));
    CHECK(r.bus.irq_asserted());                       // alternate status doesn't acknowledge
    r.bus.read_reg(kStatus);
    CHECK(!r.bus.irq_asserted());
    // Interrupt belongs to the selected device's view.
    r.bus.write_reg(kDevice, 0xB0);
    r.bus.write_reg(kCommand, 0xE7);                   // FLUSH CACHE on device 1
    CHECK(r.bus.irq_asserted());
    // Software reset in the middle of a transfer.
    r.lba(0, 4, 1);
    r.bus.write_reg(kCommand, 0x20);
    r.bus.write_device_control(SRST);
    CHECK_EQ(r.bus.read_alt_status(), static_cast<u32>(BSY));
    r.bus.write_device_control(0);
    CHECK_EQ(r.bus.selected(), 0u);                    // signature selects device 0
    CHECK_EQ(r.bus.read_reg(kStatus), static_cast<u32>(DRDY | DSC));
    CHECK_EQ(r.bus.read_reg(kSectorCount), 1u);
    CHECK_EQ(r.bus.read_reg(kError), 1u);
    // EXECUTE DEVICE DIAGNOSTIC interrupts, even with device 1 selected.
    r.bus.write_reg(kDevice, 0xB0);
    const int e = r.edges;
    r.bus.write_reg(kCommand, 0x90);
    CHECK_EQ(r.edges, e + 1);
    CHECK_EQ(r.bus.read_reg(kError), 1u);
}

TEST_CASE("devices/ata/dma_read_write_and_read_only")
{
    Rig r;
    Disk d(64);
    r.bus.attach(0, &d.dev);
    int requests = 0;
    r.bus.set_dma_request([](void* c) { ++*static_cast<int*>(c); }, &requests);
    r.lba(20, 20);                                     // more than one internal block
    r.bus.write_reg(kCommand, 0xC8);
    CHECK_EQ(requests, 1);
    CHECK_EQ(r.edges, 0);                              // no interrupt until done
    CHECK(r.bus.dma_pending());
    std::vector<u8> got(20 * 512);
    u32 moved = 0;
    while (moved < got.size()) {
        const u32 n = r.bus.dma_read(&got[moved], 700);  // odd chunk size
        if (n == 0)
            break;
        moved += n;
        if (moved < got.size())
            CHECK(!r.bus.irq_asserted());              // no interrupt mid-transfer
    }
    CHECK_EQ(moved, 20u * 512);
    CHECK_EQ(got[19 * 512 + 100], d.expected(39, 100));
    CHECK_EQ(r.edges, 1);
    CHECK(!r.bus.dma_pending());
    CHECK_EQ(r.bus.dma_read(got.data(), 4), 0u);       // nothing more

    r.lba(2, 1);
    r.bus.write_reg(kCommand, 0xCA);
    u8 blk[512];
    for (u32 i = 0; i < 512; ++i)
        blk[i] = static_cast<u8>(i ^ 0x5A);
    CHECK_EQ(r.bus.dma_write(blk, 512), 512u);
    CHECK_EQ(d.bytes[2 * 512 + 9], static_cast<u8>(9 ^ 0x5A));
    CHECK_EQ(r.edges, 2);

    Disk ro(8, true);
    r.bus.attach(1, &ro.dev);
    r.lba(0, 1, 1);
    r.bus.write_reg(kCommand, 0x30);
    CHECK_EQ(r.bus.read_reg(kError), static_cast<u32>(ABRT));
}

TEST_CASE("devices/ata/command_trace_sees_the_taskfile")
{
    Rig r;
    Disk d(64);
    r.bus.attach(0, &d.dev);
    std::vector<AtaBus::CommandTrace> log;
    r.bus.set_command_trace([](void* c, const AtaBus::CommandTrace& t) {
        static_cast<std::vector<AtaBus::CommandTrace>*>(c)->push_back(t);
    }, &log);
    r.lba(0x123456, 2);
    r.bus.write_reg(kCommand, 0x20);  // READ SECTORS
    REQUIRE(log.size() == 1u);
    CHECK_EQ(log[0].command, 0x20u);
    CHECK_EQ(log[0].count, 2u);
    CHECK_EQ(log[0].lba_low, 0x56u);
    CHECK_EQ(log[0].lba_mid, 0x34u);
    CHECK_EQ(log[0].lba_high, 0x12u);
    CHECK_EQ(log[0].device, 0xE0u);
    // Commands to an absent device are seen too (drivers probe that way).
    r.bus.write_reg(kDevice, 0xF0);
    r.bus.write_reg(kCommand, 0xEC);
    REQUIRE(log.size() == 2u);
    CHECK_EQ(log[1].command, 0xECu);
    CHECK_EQ(log[1].device, 0xF0u);
}
