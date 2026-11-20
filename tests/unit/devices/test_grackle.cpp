// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the Grackle (MPC106) configuration mechanism, accessed
// exactly the way Linux does it (arch/powerpc/sysdev/grackle.c +
// indirect_pci.c): the address is written big-endian with GRACKLE_CFA(),
// data is read/written little-endian at CONFIG_DATA + (offset & 3).

#include "core/common/endian.h"
#include "core/devices/pci/grackle.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

using namespace g3;

namespace {

// Linux: #define GRACKLE_CFA(b, d, o) (0x80 | ((b) << 8) | ((d) << 16) | (((o) & ~3) << 24))
constexpr u32 grackle_cfa(u32 b, u32 d, u32 o) { return 0x80 | (b << 8) | (d << 16) | ((o & ~3u) << 24); }

struct Fixture {
    PhysicalBus bus;
    Grackle grackle;
    bool attached;
    Fixture() { attached = grackle.attach(bus); }

    void select(u32 b, u32 devfn, u32 off) { bus.write32(Grackle::kConfigAddressBase, grackle_cfa(b, devfn, off)); }
    u32 read32(u32 b, u32 devfn, u32 off)
    {
        select(b, devfn, off);
        u32 v = 0;
        bus.read32(Grackle::kConfigDataBase, v);
        return byteswap32(v);  // in_le32
    }
    u16 read16(u32 b, u32 devfn, u32 off)
    {
        select(b, devfn, off);
        u16 v = 0;
        bus.read16(Grackle::kConfigDataBase + (off & 3), v);
        return byteswap16(v);  // in_le16
    }
    u8 read8(u32 b, u32 devfn, u32 off)
    {
        select(b, devfn, off);
        u8 v = 0;
        bus.read8(Grackle::kConfigDataBase + (off & 3), v);
        return v;
    }
    void write32(u32 b, u32 devfn, u32 off, u32 val)
    {
        select(b, devfn, off);
        bus.write32(Grackle::kConfigDataBase, byteswap32(val));  // out_le32
    }
    void write8(u32 b, u32 devfn, u32 off, u8 val)
    {
        select(b, devfn, off);
        bus.write8(Grackle::kConfigDataBase + (off & 3), val);
    }
};

struct TestDevice final : PciFunction {
    PciConfigSpace cfg;
    TestDevice()
    {
        cfg.init_header(0x106B, 0x0010, 0x01, 0xFF0000);  // arbitrary test IDs
        cfg.define_bar(0, 0x80000, false);  // 512 KB memory BAR
        cfg.define_bar(1, 0x100, true);     // 256 byte I/O BAR
    }
    u32 config_read(u32 reg) override { return cfg.read(reg); }
    void config_write(u32 reg, u32 v, u32 be) override { cfg.write(reg, v, be); }
};

} // namespace

TEST_CASE("devices/grackle/address_register_layout")
{
    Fixture f;
    REQUIRE(f.attached);
    f.select(0x12, 0x34, 0x58);
    // Little-endian CONFIG_ADDRESS: enable | bus << 16 | devfn << 8 | reg.
    CHECK_EQ(f.grackle.config_address(), 0x80123458u);
    u32 back = 0;
    f.bus.read32(Grackle::kConfigAddressBase, back);
    CHECK_EQ(back, grackle_cfa(0x12, 0x34, 0x58));  // reads back what was written
}

TEST_CASE("devices/grackle/host_bridge_identity")
{
    Fixture f;
    CHECK_EQ(f.read32(0, 0, 0x00), 0x00021057u);  // device 0x0002, vendor 0x1057
    CHECK_EQ(f.read16(0, 0, 0x00), 0x1057u);
    CHECK_EQ(f.read16(0, 0, 0x02), 0x0002u);
    CHECK_EQ(f.read8(0, 0, 0x0B), 0x06u);         // base class: bridge
    CHECK_EQ(f.read8(0, 0, 0x0A), 0x00u);         // sub-class: host bridge
    CHECK_EQ(f.read8(0, 0, 0x0E), 0x00u);         // header type 0
    // IDs are read-only.
    f.write32(0, 0, 0x00, 0xFFFFFFFFu);
    CHECK_EQ(f.read32(0, 0, 0x00), 0x00021057u);
    // PICR1 (0xA8) is plain storage for now.
    f.write32(0, 0, 0xA8, 0x12345678u);
    CHECK_EQ(f.read32(0, 0, 0xA8), 0x12345678u);
}

TEST_CASE("devices/grackle/absent_devices_and_other_buses_master_abort")
{
    Fixture f;
    CHECK_EQ(f.read32(0, 8, 0x00), 0xFFFFFFFFu);  // device 1: nothing there
    CHECK_EQ(f.read32(1, 0, 0x00), 0xFFFFFFFFu);  // bus 1: not modelled yet
    // Enable bit clear: no configuration cycle.
    f.bus.write32(Grackle::kConfigAddressBase, 0x00000000u);
    u32 v = 0;
    f.bus.read32(Grackle::kConfigDataBase, v);
    CHECK_EQ(v, 0xFFFFFFFFu);
}

TEST_CASE("devices/grackle/bar_sizing_and_assignment")
{
    Fixture f;
    TestDevice dev;
    REQUIRE(f.grackle.pci_bus().attach(0x80, &dev));  // device 16, function 0
    CHECK_EQ(f.read32(0, 0x80, 0x00), 0x0010106Bu);
    // Standard sizing: write all ones, read back the mask.
    f.write32(0, 0x80, 0x10, 0xFFFFFFFFu);
    CHECK_EQ(f.read32(0, 0x80, 0x10), 0xFFF80000u);   // 512 KB memory BAR
    f.write32(0, 0x80, 0x14, 0xFFFFFFFFu);
    CHECK_EQ(f.read32(0, 0x80, 0x14), 0xFFFFFF01u);   // 256 B I/O BAR, bit 0 = I/O
    // Assign an address.
    f.write32(0, 0x80, 0x10, 0x80800000u);
    CHECK_EQ(f.read32(0, 0x80, 0x10), 0x80800000u);
    f.write32(0, 0x80, 0x10, 0x80812345u);            // low bits are not writable
    CHECK_EQ(f.read32(0, 0x80, 0x10), 0x80800000u);
    // Command register: memory space + bus master; reserved bits stay 0.
    f.write8(0, 0x80, 0x04, 0xFF);
    CHECK_EQ(f.read8(0, 0x80, 0x04), 0x57u);
    // Unimplemented BARs read 0.
    CHECK_EQ(f.read32(0, 0x80, 0x18), 0u);
}
