// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the Heathrow-family interrupt controller, driven through
// the physical bus exactly as guest software would (big-endian CPU access to
// little-endian registers).

#include "core/common/endian.h"
#include "core/devices/macio/heathrow_pic.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"

using namespace g3;

namespace {

constexpr u32 kBase = 0xF3000000u;  // arbitrary for the test
// Linux (in_le32/out_le32) view: these helpers do what in_le32 does on a
// big-endian CPU: a 32-bit load followed by a byte swap.
u32 in_le32(PhysicalBus& bus, u32 a)
{
    u32 v = 0;
    bus.read32(a, v);
    return byteswap32(v);
}
void out_le32(PhysicalBus& bus, u32 a, u32 v) { bus.write32(a, byteswap32(v)); }

struct Fixture {
    HeathrowPic pic;
    PhysicalBus bus;
    bool cpu_irq = false;
    int transitions = 0;
    Fixture()
    {
        bus.map_mmio(kBase, HeathrowPic::kWindowSize, &pic);
        pic.set_output([](void* c, bool a) {
            auto* f = static_cast<Fixture*>(c);
            f->cpu_irq = a;
            ++f->transitions;
        }, this);
    }
};

// Register addresses as Linux computes them: bank for IRQs 0-31 at +0x20,
// bank for IRQs 32-63 at +0x10; event/enable/ack/level at +0/+4/+8/+0xC.
constexpr u32 kEvent0 = kBase + 0x20, kEnable0 = kBase + 0x24, kAck0 = kBase + 0x28, kLevel0 = kBase + 0x2C;
constexpr u32 kEvent1 = kBase + 0x10, kEnable1 = kBase + 0x14, kAck1 = kBase + 0x18;

} // namespace

TEST_CASE("devices/heathrow_pic/edge_latch_enable_ack")
{
    Fixture f;
    f.pic.set_input(5, true);
    CHECK_EQ(in_le32(f.bus, kEvent0), 1u << 5);   // latched...
    CHECK(!f.cpu_irq);                             // ...but not enabled
    out_le32(f.bus, kEnable0, 1u << 5);
    CHECK(f.cpu_irq);
    f.pic.set_input(5, false);                     // edge: stays latched
    CHECK(f.cpu_irq);
    CHECK_EQ(in_le32(f.bus, kLevel0), 0u);
    out_le32(f.bus, kAck0, 1u << 5);               // ack clears it
    CHECK_EQ(in_le32(f.bus, kEvent0), 0u);
    CHECK(!f.cpu_irq);
}

TEST_CASE("devices/heathrow_pic/edge_only_on_rising_transition")
{
    Fixture f;
    out_le32(f.bus, kEnable0, 1u << 3);
    f.pic.set_input(3, true);
    out_le32(f.bus, kAck0, 1u << 3);
    f.pic.set_input(3, true);  // still high: no new edge
    CHECK_EQ(in_le32(f.bus, kEvent0), 0u);
    CHECK(!f.cpu_irq);
    f.pic.set_input(3, false);
    f.pic.set_input(3, true);  // new rising edge
    CHECK(f.cpu_irq);
}

TEST_CASE("devices/heathrow_pic/level_triggered_input")
{
    Fixture f;
    f.pic.set_level_triggered(1ull << 20);
    out_le32(f.bus, kEnable0, 1u << 20);
    f.pic.set_input(20, true);
    CHECK(f.cpu_irq);
    CHECK_EQ(in_le32(f.bus, kEvent0), 0u);          // level inputs do not latch
    CHECK_EQ(in_le32(f.bus, kLevel0), 1u << 20);
    out_le32(f.bus, kAck0, 1u << 20);               // ack does not clear a level input
    CHECK(f.cpu_irq);
    f.pic.set_input(20, false);                     // the device deasserts
    CHECK(!f.cpu_irq);
}

TEST_CASE("devices/heathrow_pic/second_bank_is_at_0x10")
{
    Fixture f;
    f.pic.set_input(40, true);                      // IRQ 40 = bank 1, bit 8
    CHECK_EQ(in_le32(f.bus, kEvent1), 1u << 8);
    CHECK_EQ(in_le32(f.bus, kEvent0), 0u);
    out_le32(f.bus, kEnable1, 1u << 8);
    CHECK(f.cpu_irq);
    out_le32(f.bus, kAck1, 1u << 8);
    CHECK(!f.cpu_irq);
    CHECK_EQ(f.pic.enabled_mask(), 1ull << 40);
}

TEST_CASE("devices/heathrow_pic/byte_lanes_and_unused_offsets")
{
    Fixture f;
    f.pic.set_input(0, true);                       // bit 0 = byte 0 of event
    u8 b = 0;
    f.bus.read8(kEvent0, b);
    CHECK_EQ(b, 0x01u);                             // lowest byte at the lowest address
    u32 raw = 0;
    f.bus.read32(kEvent0, raw);
    CHECK_EQ(raw, 0x01000000u);                     // plain lwz sees it byte-swapped
    f.bus.read32(kBase + 0x00, raw);
    CHECK_EQ(raw, 0u);                              // unused part of the window
    // Output callback fired once per transition only.
    const int before = f.transitions;
    out_le32(f.bus, kEnable0, 1);
    out_le32(f.bus, kEnable0, 1);
    CHECK_EQ(f.transitions, before + 1);
}

TEST_CASE("devices/heathrow_pic/mode_bit_is_not_a_source")
{
    // Bit 31 of the IRQ 0-31 enable register is IntMode (DingusPPC's model;
    // Mac OS sets it): enabling it alone never interrupts the CPU.
    Fixture f;
    f.pic.set_input(31, true);
    out_le32(f.bus, kEnable0, 0x80000000u);
    CHECK(f.pic.mode_68k());
    CHECK(!f.cpu_irq);
    // It reads back in both enable registers; the second copy is read-only.
    CHECK_EQ(in_le32(f.bus, kEnable0), 0x80000000u);
    CHECK_EQ(in_le32(f.bus, kEnable1), 0x80000000u);
    out_le32(f.bus, kEnable1, 0x00000004u);
    CHECK_EQ(in_le32(f.bus, kEnable1), 0x80000004u);
    out_le32(f.bus, kEnable0, 0);
    CHECK(!f.pic.mode_68k());
    CHECK_EQ(in_le32(f.bus, kEnable1), 0x00000004u);
    // Writing bit 31 there neither sets the mode nor enables IRQ 63.
    f.pic.set_input(63, true);
    out_le32(f.bus, kEnable1, 0x80000000u);
    CHECK_EQ(in_le32(f.bus, kEnable1), 0u);
    CHECK(!f.pic.mode_68k());
    CHECK(!f.cpu_irq);
}

TEST_CASE("devices/heathrow_pic/mode_68k_clear_all_and_both_edges")
{
    Fixture f;
    out_le32(f.bus, kEnable0, 0x80000000u | (1u << 18));
    out_le32(f.bus, kEnable1, 1u << 2);
    f.pic.set_input(18, true);
    f.pic.set_input(34, true);
    CHECK(f.cpu_irq);
    // Writing bit 31 to the IRQ 0-31 ack register clears all events, both banks.
    out_le32(f.bus, kAck0, 0x80000000u);
    CHECK_EQ(in_le32(f.bus, kEvent0), 0u);
    CHECK_EQ(in_le32(f.bus, kEvent1), 0u);
    CHECK(!f.cpu_irq);
    // In 68K mode the line going low latches an event too.
    f.pic.set_input(18, false);
    CHECK_EQ(in_le32(f.bus, kEvent0), 1u << 18);
    CHECK(f.cpu_irq);
    out_le32(f.bus, kAck0, 1u << 18);
    CHECK(!f.cpu_irq);
    // In native mode a falling edge latches nothing, and bit 31 clears nothing else.
    out_le32(f.bus, kEnable0, 1u << 18);
    f.pic.set_input(18, true);
    out_le32(f.bus, kAck0, 0x80000000u);
    CHECK_EQ(in_le32(f.bus, kEvent0), 1u << 18);
    out_le32(f.bus, kAck0, 1u << 18);
    f.pic.set_input(18, false);
    CHECK_EQ(in_le32(f.bus, kEvent0), 0u);
    CHECK(!f.cpu_irq);
}
