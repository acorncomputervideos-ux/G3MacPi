// SPDX-License-Identifier: GPL-3.0-or-later
// The MESH SCSI cell with an empty bus (mesh.h): register layout, Linux's
// initialisation sequence, a selection that times out, write-1-to-clear
// bits, and its interrupt reaching Paddington's PIC on input 12.

#include "core/devices/macio/mesh.h"
#include "core/machine/yosemite.h"
#include "tests/framework/g3test.h"

using namespace g3;

namespace {
// Linux drivers/scsi/mesh.h: register n at n * 0x10.
enum : u32 { kCountLo = 0x00, kCountHi = 0x10, kFifo = 0x20, kSeq = 0x30, kBus0 = 0x40, kBus1 = 0x50,
             kFifoCount = 0x60, kExc = 0x70, kErr = 0x80, kMask = 0x90, kIntr = 0xA0, kSrcId = 0xB0,
             kDestId = 0xC0, kSync = 0xD0, kMeshId = 0xE0, kSelTo = 0xF0 };
u8 rd(MacioMesh& m, u32 off) { return static_cast<u8>(m.mmio_read(off, 1)); }
void wr(MacioMesh& m, u32 off, u8 v) { m.mmio_write(off, 1, v); }
} // namespace

TEST_CASE("devices/mesh/registers_and_linux_init")
{
    MacioMesh m;
    int irq_changes = 0;
    bool line = false;
    struct Ctx { int* n; bool* l; } ctx{&irq_changes, &line};
    m.set_irq([](void* c, bool a) { auto* x = static_cast<Ctx*>(c); ++*x->n; *x->l = a; }, &ctx);
    CHECK_EQ(rd(m, kMeshId), 4u);
    // As Linux mesh_init: clear exception/error, reset, mask, IDs, timeout, async.
    wr(m, kExc, 0xFF);
    wr(m, kErr, 0xFF);
    wr(m, kSeq, 0x0E);                                     // reset MESH: command done
    CHECK_EQ(rd(m, kIntr), 0x01u);
    CHECK(!line);                                          // masked
    wr(m, kMask, 0x07);
    CHECK(line);                                           // unmasked: the pending bit drives the line
    wr(m, kIntr, 0xFF);                                    // write 1 to clear
    CHECK(!line);
    CHECK_EQ(rd(m, kIntr), 0u);
    wr(m, kSrcId, 7);
    wr(m, kSelTo, 25);
    wr(m, kSync, 2);
    CHECK(rd(m, kSrcId) == 7 && rd(m, kSelTo) == 25 && rd(m, kSync) == 2 && rd(m, kMask) == 7);
    // Bus reset: RST shows in bus status 1 while asserted.
    wr(m, kBus1, 0x80);
    CHECK_EQ(rd(m, kBus1), 0x80u);
    wr(m, kBus1, 0);
    CHECK_EQ(rd(m, kBus1), 0u);
    CHECK_EQ(rd(m, kBus0), 0u);
    // Count and FIFO.
    wr(m, kCountLo, 0x34);
    wr(m, kCountHi, 0x12);
    CHECK(rd(m, kCountLo) == 0x34 && rd(m, kCountHi) == 0x12);
    wr(m, kFifo, 0xAA);
    wr(m, kFifo, 0xBB);
    CHECK_EQ(rd(m, kFifoCount), 2u);
    CHECK_EQ(rd(m, kFifo), 0xAAu);
    wr(m, kSeq, 0x0F);                                     // flush FIFO
    CHECK_EQ(rd(m, kFifoCount), 0u);
    CHECK_EQ(rd(m, kIntr), 0x01u);
    wr(m, kIntr, 0x01);
    // Arbitrate, then select target 0: nobody answers -> selection timeout.
    wr(m, kDestId, 0);
    wr(m, kSeq, 0x01);
    CHECK_EQ(rd(m, kIntr), 0x01u);
    CHECK_EQ(rd(m, kExc), 0u);
    wr(m, kIntr, 0x07);
    wr(m, kSeq, 0x02 | 0x20);                              // select with ATN
    CHECK_EQ(rd(m, kExc), 0x01u);
    CHECK_EQ(rd(m, kIntr) & 0x02, 0x02u);                 // exception interrupt
    CHECK(line);
    wr(m, kExc, 0x01);
    wr(m, kIntr, 0x07);
    CHECK_EQ(rd(m, kExc), 0u);
    CHECK(!line);
    // A bus phase without a connection: sequence error.
    wr(m, kSeq, 0x03);
    CHECK_EQ(rd(m, kErr) & 0x10, 0x10u);
    CHECK_EQ(rd(m, kIntr) & 0x04, 0x04u);
    CHECK(irq_changes >= 4);
}

TEST_CASE("devices/mesh/in_paddington_on_irq_12")
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
    REQUIRE(p.mapped_base() == 0x80800000u);
    u8 id = 0;
    REQUIRE(m.bus().read8(0x80810000u + kMeshId, id));    // the machine check Mac OS hit: now answered
    CHECK_EQ(id, 4u);
    u8 bs0 = 0xFF;
    REQUIRE(m.bus().read8(0x80810040u, bs0));
    CHECK_EQ(bs0, 0u);
    // Enable PIC input 12, then make MESH interrupt.
    p.pic().mmio_write(0x24, 4, 0x00100000u);              // enable bank 0 bit 12 (little-endian register)
    REQUIRE(m.bus().write8(0x80810000u + kMask, 0x07));
    REQUIRE(m.bus().write8(0x80810000u + kSeq, 0x0E));
    CHECK(p.pic().output());
    REQUIRE(m.bus().write8(0x80810000u + kIntr, 0xFF));   // MESH drops its line
    p.pic().mmio_write(0x28, 4, 0x00100000u);              // acknowledge the latched event (edge input)
    CHECK(!p.pic().output());
    // The cell moves with the BAR and is gone when decoding is off.
    p.config_write(0x04, 0x0, 0x3);
    CHECK(!m.bus().read8(0x80810000u + kMeshId, id));
}
