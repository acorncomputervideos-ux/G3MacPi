// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the PCI-to-PCI bridge (type 1 header) and configuration
// routing through it, accessed through the Grackle ports like Linux does.

#include "core/common/endian.h"
#include "core/devices/pci/grackle.h"
#include "core/devices/pci/pci_bridge.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

using namespace g3;

namespace {

constexpr u32 grackle_cfa(u32 b, u32 d, u32 o) { return 0x80 | (b << 8) | (d << 16) | ((o & ~3u) << 24); }

struct Leaf final : PciFunction {
    PciConfigSpace cfg;
    explicit Leaf(u16 device) { cfg.init_header(0x106B, device, 0, 0xFF0000); }
    u32 config_read(u32 r) override { return cfg.read(r); }
    void config_write(u32 r, u32 v, u32 be) override { cfg.write(r, v, be); }
};

struct Fixture {
    PhysicalBus bus;
    Grackle grackle;
    PciToPciBridge bridge = PciToPciBridge::dec21154();
    Leaf behind{0x0017};       // a device on the secondary bus
    PciToPciBridge bridge2 = PciToPciBridge::dec21154();
    Leaf deeper{0x0099};       // behind a second bridge
    Fixture()
    {
        grackle.attach(bus);
        grackle.pci_bus().attach(0x0D << 3, &bridge);       // 00:0D.0 (B&W, to verify)
        bridge.secondary_bus().attach(5 << 3, &behind);     // dev 5 on the secondary bus
        bridge.secondary_bus().attach(9 << 3, &bridge2);
        bridge2.secondary_bus().attach(0, &deeper);
    }
    u32 rd(u32 b, u32 devfn, u32 off)
    {
        bus.write32(Grackle::kConfigAddressBase, grackle_cfa(b, devfn, off));
        u32 v = 0;
        bus.read32(Grackle::kConfigDataBase, v);
        return byteswap32(v);
    }
    void wr(u32 b, u32 devfn, u32 off, u32 val)
    {
        bus.write32(Grackle::kConfigAddressBase, grackle_cfa(b, devfn, off));
        bus.write32(Grackle::kConfigDataBase, byteswap32(val));
    }
    u8 rd8(u32 b, u32 devfn, u32 off)
    {
        bus.write32(Grackle::kConfigAddressBase, grackle_cfa(b, devfn, off));
        u8 v = 0;
        bus.read8(Grackle::kConfigDataBase + (off & 3), v);
        return v;
    }
};

constexpr u32 kBridge = 0x0D << 3;

} // namespace

TEST_CASE("devices/pci_bridge/type1_header")
{
    Fixture f;
    CHECK_EQ(f.rd(0, kBridge, 0x00), 0x00261011u);  // DEC 21154
    CHECK_EQ(f.rd8(0, kBridge, 0x0E), 0x01u);       // header type 1
    CHECK_EQ(f.rd(0, kBridge, 0x08) >> 8, 0x060400u);
    // Bus numbers are writable.
    f.wr(0, kBridge, 0x18, 0x00050100u);            // primary 0, secondary 1, subordinate 5
    CHECK_EQ(f.rd(0, kBridge, 0x18) & 0x00FFFFFFu, 0x00050100u);
    // Memory window: low nibble read-only.
    f.wr(0, kBridge, 0x20, 0x80F08080u);
    CHECK_EQ(f.rd(0, kBridge, 0x20), 0x80F08080u & 0xFFF0FFF0u);
}

TEST_CASE("devices/pci_bridge/config_routing")
{
    Fixture f;
    // Unconfigured bridge (secondary = 0): nothing behind it is reachable.
    CHECK_EQ(f.rd(1, 5 << 3, 0x00), 0xFFFFFFFFu);
    // Configure like firmware: secondary 1, subordinate 2.
    f.wr(0, kBridge, 0x18, 0x00020100u);
    CHECK_EQ(f.rd(1, 5 << 3, 0x00), 0x0017106Bu);   // 01:05.0
    CHECK_EQ(f.rd(1, 6 << 3, 0x00), 0xFFFFFFFFu);   // empty slot on bus 1
    CHECK_EQ(f.rd(3, 0, 0x00), 0xFFFFFFFFu);        // outside 1..2
    // The second bridge sits on bus 1; give it bus 2.
    f.wr(1, 9 << 3, 0x18, 0x00020201u);
    CHECK_EQ(f.rd(2, 0, 0x00), 0x0099106Bu);        // 02:00.0 through two bridges
    // Writes are routed too.
    f.wr(1, 5 << 3, 0x3C, 0x0000002Au);
    CHECK_EQ(f.rd(1, 5 << 3, 0x3C) & 0xFF, 0x2Au);
}
