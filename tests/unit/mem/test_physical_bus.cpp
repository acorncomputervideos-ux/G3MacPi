// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/mem/physical_bus.

#include "core/mem/physical_bus.h"
#include "core/cpu/ppc750/ppc750.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;

namespace {

struct RecordingDevice final : MmioDevice {
    struct Access { bool write; u32 offset, size, value; };
    std::vector<Access> log;
    u32 read_value = 0x12345678;
    u32 mmio_read(u32 offset, u32 size) override
    {
        log.push_back({false, offset, size, 0});
        return size == 4 ? read_value : (size == 2 ? read_value & 0xFFFF : read_value & 0xFF);
    }
    void mmio_write(u32 offset, u32 size, u32 value) override
    {
        log.push_back({true, offset, size, value});
    }
};

} // namespace

TEST_CASE("mem/bus/ram_big_endian_and_host_pointer")
{
    static u8 ram[0x10000];
    PhysicalBus bus;
    REQUIRE(bus.map_ram(0, ram, sizeof ram));
    REQUIRE(bus.write32(0x100, 0xAABBCCDDu));
    CHECK_EQ(ram[0x100], 0xAAu);
    CHECK_EQ(ram[0x103], 0xDDu);
    u16 h = 0;
    REQUIRE(bus.read16(0x102, h));
    CHECK_EQ(h, 0xCCDDu);
    CHECK(bus.host_pointer(0x100, 4) == ram + 0x100);
    CHECK(bus.host_pointer(0xFFFE, 4) == nullptr);  // runs off the region
}

TEST_CASE("mem/bus/overlap_and_bad_mappings_rejected")
{
    static u8 a[0x2000], b[0x2000];
    PhysicalBus bus;
    REQUIRE(bus.map_ram(0x1000, a, sizeof a));
    CHECK(!bus.map_ram(0x2000, b, sizeof b));      // overlaps 0x1000-0x2FFF
    CHECK(!bus.map_ram(0x0800, b, 0x1000));        // overlaps the start
    CHECK(bus.map_ram(0x3000, b, sizeof b));       // adjacent is fine
    CHECK(!bus.map_ram(0xFFFFF000u, b, 0x2000));   // wraps past 4 GB
    CHECK(!bus.map_ram(0x8000, b, 0));             // empty
    CHECK(bus.map_ram(0xFFFFE000u, b, 0x2000));    // ends exactly at 4 GB
}

TEST_CASE("mem/bus/rom_ignores_writes")
{
    static const u8 rom[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    PhysicalBus bus;
    REQUIRE(bus.map_rom(0xFFF00000u, rom, sizeof rom));
    CHECK(bus.write32(0xFFF00000u, 0));
    u32 v = 0;
    REQUIRE(bus.read32(0xFFF00000u, v));
    CHECK_EQ(v, 0x01020304u);
    CHECK_EQ(bus.rom_writes_ignored(), 1u);
}

TEST_CASE("mem/bus/mmio_offsets_and_sizes")
{
    RecordingDevice dev;
    PhysicalBus bus;
    REQUIRE(bus.map_mmio(0xF3000000u, 0x1000, &dev));
    u32 v = 0;
    REQUIRE(bus.read32(0xF3000020u, v));
    CHECK_EQ(v, 0x12345678u);
    REQUIRE(bus.write16(0xF3000102u, 0xBEEF));
    u8 b = 0;
    REQUIRE(bus.read8(0xF3000FFFu, b));
    REQUIRE(dev.log.size() == 3);
    CHECK_EQ(dev.log[0].offset, 0x20u);
    CHECK_EQ(dev.log[0].size, 4u);
    CHECK(dev.log[1].write);
    CHECK_EQ(dev.log[1].offset, 0x102u);
    CHECK_EQ(dev.log[1].size, 2u);
    CHECK_EQ(dev.log[1].value, 0xBEEFu);
    CHECK_EQ(dev.log[2].offset, 0xFFFu);
    CHECK_EQ(b, 0x78u);
}

TEST_CASE("mem/bus/unmapped_policies")
{
    PhysicalBus bus;
    u32 v = 0;
    CHECK(!bus.read32(0x40000000u, v));  // default: bus error
    CHECK_EQ(bus.unmapped_accesses(), 1u);
    CHECK_EQ(bus.last_unmapped_address(), 0x40000000u);
    bus.set_unmapped_policy(PhysicalBus::UnmappedPolicy::AllOnes);
    CHECK(bus.read32(0x40000000u, v));
    CHECK_EQ(v, 0xFFFFFFFFu);
    u16 h = 0;
    CHECK(bus.read16(0x40000000u, h));
    CHECK_EQ(h, 0xFFFFu);
    CHECK(bus.write32(0x40000000u, 1));
}

TEST_CASE("mem/bus/access_straddling_two_regions")
{
    static u8 ram[0x1000];
    RecordingDevice dev;
    dev.read_value = 0xAB;
    PhysicalBus bus;
    REQUIRE(bus.map_ram(0x0000, ram, sizeof ram));
    REQUIRE(bus.map_mmio(0x1000, 0x100, &dev));
    ram[0xFFE] = 0x11;
    ram[0xFFF] = 0x22;
    u32 v = 0;
    REQUIRE(bus.read32(0xFFE, v));  // 2 bytes RAM + 2 bytes device
    CHECK_EQ(v, 0x1122ABABu);
    REQUIRE(dev.log.size() == 2);
    CHECK_EQ(dev.log[0].offset, 0u);
    CHECK_EQ(dev.log[1].offset, 1u);
    // Half mapped, half unmapped with the bus-error policy: fails.
    CHECK(!bus.read32(0x10FE, v));
}

TEST_CASE("mem/bus/cpu_runs_from_bus")
{
    // The CPU can execute from RAM mapped through the bus.
    static u8 ram[0x4000];
    PhysicalBus bus;
    REQUIRE(bus.map_ram(0, ram, sizeof ram));
    bus.write32(0x1000, 0x38600029u);  // li r3,41
    bus.write32(0x1004, 0x38630001u);  // addi r3,r3,1
    ppc::Ppc750 cpu(bus);
    cpu.state().pc = 0x1000;
    cpu.state().msr = 0;
    cpu.run(2);
    CHECK_EQ(cpu.state().gpr[3], 42u);
}

TEST_CASE("mem/bus/unmap_and_remap")
{
    RecordingDevice dev;
    PhysicalBus bus;
    REQUIRE(bus.map_mmio(0x80000000u, 0x100, &dev));
    u32 v = 0;
    CHECK(bus.read32(0x80000000u, v));
    CHECK(bus.unmap(0x80000000u));
    CHECK(!bus.unmap(0x80000000u));
    CHECK(!bus.read32(0x80000000u, v));          // gone (bus error)
    REQUIRE(bus.map_mmio(0x90000000u, 0x100, &dev));
    CHECK(bus.read32(0x90000010u, v));
    CHECK_EQ(dev.log.back().offset, 0x10u);
}


namespace {
// Records the RAM writes a bus reports (the block cache's view).
struct WriteRecord {
    u32 count = 0, lo = 0xFFFFFFFFu, hi = 0;
    static void hear(void* ctx, u32 pa, u32 len)
    {
        auto* w = static_cast<WriteRecord*>(ctx);
        ++w->count;
        if (pa < w->lo) w->lo = pa;
        if (pa + len > w->hi) w->hi = pa + len;
    }
};
} // namespace

TEST_CASE("mem/bus/write_observer_hears_ram_writes_only")
{
    std::vector<u8> ram(0x10000), rom(0x1000);
    PhysicalBus bus;
    REQUIRE(bus.map_ram(0, ram.data(), 0x10000));
    REQUIRE(bus.map_rom(0xFFF00000u, rom.data(), 0x1000));
    WriteRecord heard;
    bus.set_write_observer(WriteRecord::hear, &heard);
    u32 v;
    bus.read32(0x100, v);
    CHECK_EQ(heard.count, 0u);
    bus.write16(0x1FFE, 0x1234);
    CHECK_EQ(heard.count, 1u);
    CHECK_EQ(heard.lo, 0x1FFEu);
    CHECK_EQ(heard.hi, 0x2000u);
    bus.write32(0xFFF00000u, 1);  // ROM: ignored, nothing changed
    bus.write8(0x80000000u, 1);   // unmapped
    CHECK_EQ(heard.count, 1u);
    bus.notify_write(0x4000, 0x200);  // DMA through host_pointer
    CHECK_EQ(heard.count, 2u);
    CHECK_EQ(heard.hi, 0x4200u);
}
