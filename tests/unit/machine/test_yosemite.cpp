// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whole-machine test: a PowerPC program running on the assembled Yosemite
// machine enumerates PCI through the Grackle, configures the DEC 21154,
// finds Paddington behind it, assigns its BAR, enables a VIA timer interrupt
// through the interrupt controller, and handles the interrupt.

#include "core/machine/yosemite.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <vector>

using namespace g3;
using namespace asm_ppc;

namespace {

constexpr u32 kResults = 0x2000;
constexpr u32 kMain = 0x3000;

void load(YosemiteMachine& m, u32 addr, const std::vector<u32>& code)
{
    for (u32 w : code) {
        m.bus().write32(addr, w);
        addr += 4;
    }
}

} // namespace

TEST_CASE("machine/yosemite/pci_enumeration_and_via_interrupt")
{
    static u8 ram[1024 * 1024];
    YosemiteConfig cfg;
    cfg.ram = ram;
    cfg.ram_size = sizeof ram;
    YosemiteMachine m(cfg);
    REQUIRE(m.ok());

    // Registers: r20 CONFIG_ADDRESS, r21 CONFIG_DATA, r30 results,
    // r22 VIA base, r23 interrupt-controller base.
    // CONFIG_ADDRESS values are little-endian (enable | bus<<16 | devfn<<8 | reg),
    // written with stwbrx, data read/written with lwbrx/stwbrx.
    const std::vector<u32> main_code = {
        lis(20, 0xFEC0), lis(21, 0xFEE0), li(30, kResults),
        // 1. Host bridge ID (00:00.0 reg 0).
        lis(3, 0x8000), stwbrx(3, 0, 20), lwbrx(4, 0, 21), stw(4, 0, 30),
        // 2. Bridge 00:0D.0 reg 0x18: primary 0, secondary 1, subordinate 1.
        lis(3, 0x8000), ori(3, 3, 0x6818), stwbrx(3, 0, 20),
        lis(5, 0x0001), ori(5, 5, 0x0100), stwbrx(5, 0, 21),
        // 3. ID of 01:05.0 (Paddington).
        lis(3, 0x8001), ori(3, 3, 0x2800), stwbrx(3, 0, 20), lwbrx(6, 0, 21), stw(6, 4, 30),
        // 4. BAR0 = 0x80800000.
        lis(3, 0x8001), ori(3, 3, 0x2810), stwbrx(3, 0, 20), lis(5, 0x8080), stwbrx(5, 0, 21),
        // 5. Command register: memory space enable.
        lis(3, 0x8001), ori(3, 3, 0x2804), stwbrx(3, 0, 20), li(5, 2), stwbrx(5, 0, 21),
        // 6. VIA at BAR + 0x16000: read IER (+0x1C00).
        lis(22, 0x8081), ori(22, 22, 0x6000), lbz(7, 0x1C00, 22), stw(7, 8, 30),
        // 7. Interrupt controller at BAR + 0: enable IRQ 18 (bank 0 enable, +0x24).
        lis(23, 0x8080), lis(5, 0x0004), li(6, 0x24), stwbrx(5, 23, 6),
        // 8. VIA: enable the T2 interrupt; start T2 with 100 ticks.
        li(5, 0xA0), stb(5, 0x1C00, 22),
        li(5, 100), stb(5, 0x1000, 22), li(5, 0), stb(5, 0x1200, 22),
        // 9. MSR[EE] = 1, then spin.
        mfmsr(5), ori(5, 5, 0x8000), mtmsr(5),
        b(0),
    };
    // External interrupt handler at 0x500.
    const std::vector<u32> handler = {
        li(6, 0x20), lwbrx(8, 23, 6), stw(8, 12, 30),   // event register (bank 0)
        lbz(9, 0x1000, 22),                              // read T2CL: clears the VIA flag
        li(6, 0x28), stwbrx(8, 23, 6),                   // ack the controller
        li(10, 1), stw(10, 16, 30),
        rfi(),
    };
    load(m, kMain, main_code);
    load(m, 0x500, handler);
    m.cpu().state().pc = kMain;
    m.cpu().state().msr = 0;
    m.cpu().state().spr[ppc::spr::DEC] = 0x7FFFFFFFu;  // no decrementer interrupt

    m.run_for(1000000000ull);  // 1 ms of guest time

    auto word = [&](u32 off) { u32 v = 0; m.bus().read32(kResults + off, v); return v; };
    CHECK_EQ(word(0), 0x00021057u);   // MPC106
    CHECK_EQ(word(4), 0x0017106Bu);   // Paddington, found behind the bridge
    CHECK_EQ(word(8), 0x80u);         // VIA IER read through the mac-io BAR
    CHECK_EQ(word(12), 1u << 18);     // interrupt controller saw the VIA on input 18
    CHECK_EQ(word(16), 1u);           // handler ran
    CHECK_EQ(m.macio().mapped_base(), 0x80800000u);
    CHECK(!m.macio().pic().output());
    CHECK_EQ(m.cpu().state().pc, kMain + 4u * (static_cast<u32>(main_code.size()) - 1));
}

TEST_CASE("machine/yosemite/macio_moves_with_its_bar")
{
    static u8 ram[64 * 1024];
    YosemiteConfig cfg;
    cfg.ram = ram;
    cfg.ram_size = sizeof ram;
    YosemiteMachine m(cfg);
    REQUIRE(m.ok());
    Paddington& p = m.macio();
    p.config_write(0x10, 0x80800000u, 0xF);
    CHECK_EQ(p.mapped_base(), 0u);              // memory decoding still off
    p.config_write(0x04, 0x2, 0x3);
    CHECK_EQ(p.mapped_base(), 0x80800000u);
    p.config_write(0x10, 0x90000000u, 0xF);     // software moves the device
    CHECK_EQ(p.mapped_base(), 0x90000000u);
    u8 v = 0;
    CHECK(m.bus().read8(0x90000000u + Paddington::kViaOffset + 0x1C00, v));
    CHECK_EQ(v, 0x80u);
    CHECK(!m.bus().read8(0x80800000u + Paddington::kViaOffset + 0x1C00, v));  // old place gone
    p.config_write(0x04, 0x0, 0x3);             // decoding off: unmapped
    CHECK_EQ(p.mapped_base(), 0u);
}

TEST_CASE("machine/yosemite/macio_dbdma_cell_and_conflict_rollback")
{
    static u8 ram[64 * 1024];
    static u8 other[0x1000];
    YosemiteConfig cfg;
    cfg.ram = ram;
    cfg.ram_size = sizeof ram;
    YosemiteMachine m(cfg);
    REQUIRE(m.ok());
    Paddington& p = m.macio();
    p.config_write(0x10, 0x80800000u, 0xF);
    p.config_write(0x04, 0x2, 0x3);
    // DBDMA channel 4 cmdptr (little-endian register) through the bus.
    REQUIRE(m.bus().write32(0x80800000u + Paddington::kDbdmaOffset + 0x40C, 0x00100000u));
    CHECK_EQ(p.dbdma().channel(4).command_pointer(), 0x00001000u);  // byte-swapped: 0x1000

    // Something else already decodes where the VIA would land: the move
    // fails, mac-io ends up unmapped, and the other region is untouched.
    other[0] = 0x5A;
    REQUIRE(m.bus().map_ram(0xA0000000u + Paddington::kViaOffset, other, sizeof other));
    p.config_write(0x10, 0xA0000000u, 0xF);
    CHECK_EQ(p.mapped_base(), 0u);
    u8 v = 0;
    CHECK(m.bus().read8(0xA0000000u + Paddington::kViaOffset, v));
    CHECK_EQ(v, 0x5Au);
    CHECK(!m.bus().read8(0xA0000000u + Paddington::kDbdmaOffset, v));  // rolled back
    CHECK(!m.bus().read8(0xA0000000u + Paddington::kPicOffset + 0x20, v));
}

TEST_CASE("machine/yosemite/macio_nvram_cell")
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
    const u32 nv = 0x80800000u + Paddington::kNvramOffset;
    REQUIRE(m.bus().write8(nv + 0x1FFF * 16, 0x5A));   // last NVRAM byte
    CHECK_EQ(p.nvram().byte(0x1FFF), 0x5Au);
    p.config_write(0x04, 0x0, 0x3);
    u8 v = 0;
    CHECK(!m.bus().read8(nv, v));                       // unmapped with the BAR
    CHECK_EQ(p.nvram().byte(0x1FFF), 0x5Au);            // contents kept
}
