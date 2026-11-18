// SPDX-License-Identifier: GPL-3.0-or-later
// Virtual-Mode memory management: the translations table, the hashed page
// table (checked by the emulated 750's own MMU and by the worked example of
// tests/unit/cpu/test_ppc750_mmu.cpp), and the mmu / memory package methods.

#include "core/cpu/ppc750/ppc750.h"
#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/mmu.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"

#include <memory>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::of;

namespace {

std::vector<u32> flat(const Translations& t)
{
    std::vector<u32> v;
    for (u32 i = 0; i < t.count(); ++i) {
        v.push_back(t[i].virt);
        v.push_back(t[i].size);
        v.push_back(t[i].phys);
        v.push_back(t[i].mode);
    }
    return v;
}
using V = std::vector<u32>;

} // namespace

TEST_CASE("firmware/mmu/translations_map_unmap_split")
{
    Translations t;
    REQUIRE(t.map(0x00000000, 0x00000000, 0x01000000, 0x12));       // RAM 1:1
    REQUIRE(t.map(0x00400000, 0x10000000, 0x3000, 0x10));             // an alias
    CHECK(flat(t) == V({0, 0x01000000, 0, 0x12, 0x10000000, 0x3000, 0x00400000, 0x10}));
    // Page rounding: the start goes down, the end up.
    REQUIRE(t.map(0x00800123, 0x20000123, 0x1000, 0x10));
    CHECK(t[2].virt == 0x20000000u && t[2].size == 0x2000u && t[2].phys == 0x00800000u);
    // Misaligned within the page relative to each other: refused.
    CHECK(!t.map(0x00800100, 0x30000200, 0x100, 0x10));
    // Unmapping the middle of the RAM entry splits it in two.
    t.unmap(0x00200000, 0x00100000);
    CHECK(t.count() == 4u);
    CHECK(t[0].virt == 0 && t[0].size == 0x00200000u);
    CHECK(t[1].virt == 0x00300000u && t[1].size == 0x00D00000u && t[1].phys == 0x00300000u);
    u32 phys = 0, mode = 0;
    CHECK(!t.translate(0x00250000, phys, mode));
    REQUIRE(t.translate(0x10002ABC, phys, mode));
    CHECK(phys == 0x00402ABCu && mode == 0x10u);
    REQUIRE(t.translate(0x00FFFFFC, phys, mode));
    CHECK(phys == 0x00FFFFFCu);
    // Mapping over existing entries replaces them.
    REQUIRE(t.map(0x00700000, 0x10001000, 0x1000, 0x2A));
    REQUIRE(t.translate(0x10001004, phys, mode));
    CHECK(phys == 0x00700004u && mode == 0x2Au);
    REQUIRE(t.translate(0x10000004, phys, mode));
    CHECK(phys == 0x00400004u);
    REQUIRE(t.translate(0x10002004, phys, mode));
    CHECK(phys == 0x00402004u);
    // modify changes the mode only.
    REQUIRE(t.modify(0x10000000, 0x3000, 0x6A));
    REQUIRE(t.translate(0x10002004, phys, mode));
    CHECK(phys == 0x00402004u && mode == 0x6Au);
    CHECK(!t.modify(0x50000000, 0x1000, 0x10));
    // Ascending order is kept.
    for (u32 i = 1; i < t.count(); ++i)
        CHECK(t[i - 1].virt + t[i - 1].size <= t[i].virt);
    // Ranges that run past 4 GB are refused.
    CHECK(!t.map(0, 0xFFFFF000u, 0x2000, 0x10));
}

TEST_CASE("firmware/mmu/translations_table_full")
{
    Translations t;
    bool ok = true;
    for (u32 i = 0; i < Translations::kMax && ok; ++i)
        ok = t.map(i * 0x2000, i * 0x2000, 0x1000, 0x10);
    CHECK(!ok || t.count() <= Translations::kMax);
    // Once full, a map that would need a new entry is refused without damage.
    const u32 n = t.count();
    CHECK(!t.map(0x10000000, 0x10000000, 0x1000, 0x10));
    CHECK(t.count() == n);
}

TEST_CASE("firmware/mmu/page_table_matches_the_cpu")
{
    constexpr u32 kRam = 4u << 20;
    std::unique_ptr<u8[]> ram(new u8[kRam]());
    FlatMemory mem(ram.get(), kRam);
    ppc::Ppc750 cpu(mem);
    // The worked example of test_ppc750_mmu.cpp: HTAB at 0x10000 (64 KB), VSID
    // 0x123, EA 0x3000 -> primary PTEG 0x14800, word 0 0x80009180.
    PageTable pt(mem, 0x00010000u, 0x10000u);
    REQUIRE(pt.clear());
    CHECK(pt.sdr1() == 0x00010000u);
    REQUIRE(pt.insert(0x123, 0x00003000u, 0x00005000u, 0x12));
    u32 w0 = 0, w1 = 0;
    mem.read32(0x14800, w0);
    mem.read32(0x14804, w1);
    CHECK(w0 == 0x80009180u);
    CHECK(w1 == 0x00005012u);                                   // RPN | M | PP=10
    // Fill the primary group: the ninth PTE goes to the secondary group (0x1B7C0).
    for (u32 i = 1; i < 8; ++i)
        REQUIRE(pt.insert(0x123, 0x00003000u, 0x00005000u, 0x12));
    REQUIRE(pt.insert(0x123, 0x00003000u, 0x00006000u, 0x12));
    mem.read32(0x1B7C0, w0);
    CHECK(w0 == 0x800091C0u);
    for (u32 i = 1; i < 8; ++i)
        REQUIRE(pt.insert(0x123, 0x00003000u, 0x00006000u, 0x12));
    CHECK(!pt.insert(0x123, 0x00003000u, 0x00007000u, 0x12));      // both groups full
    CHECK(pt.entries() == 16u);

    // A 128 KB table elsewhere, many pages, every one found by the CPU.
    PageTable big(mem, 0x00100000u, 0x20000u);
    REQUIRE(big.clear());
    for (u32 sr = 0; sr < 16; ++sr)
        cpu.state().sr[sr] = 0x400 + sr;                         // VSID 0x400 + n, Ks = 0
    cpu.state().spr[ppc::spr::SDR1] = big.sdr1();
    CHECK(big.sdr1() == 0x00100001u);
    for (u32 i = 0; i < 300; ++i) {
        const u32 ea = 0x10000000u + i * 0x1000u + (i % 3) * 0x01000000u;
        REQUIRE(big.insert(0x400 + (ea >> 28), ea, 0x00200000u + i * 0x1000u, 0x12));
    }
    cpu.state().msr = ppc::msr::DR;
    cpu.flush_tlb();
    for (u32 i = 0; i < 300; ++i) {
        const u32 ea = 0x10000000u + i * 0x1000u + (i % 3) * 0x01000000u;
        u32 pa = 0;
        REQUIRE(cpu.probe_translate(ea + 0x123, false, pa));
        CHECK(pa == 0x00200000u + i * 0x1000u + 0x123);
    }
    u32 pa = 0;
    CHECK(!cpu.probe_translate(0x50000000u, false, pa));         // nothing mapped there
    // The probe left R/C bits alone; a real load sets R.
    PageTable one(mem, 0x00300000u, 0x10000u);
    REQUIRE(one.clear());
    cpu.state().spr[ppc::spr::SDR1] = one.sdr1();
    REQUIRE(one.insert(0x400 + 1, 0x10000000u, 0x00001000u, 0x12));
    cpu.flush_tlb();
    REQUIRE(cpu.probe_translate(0x10000000u, true, pa));
    bool r_set = false;
    for (u32 a = 0x00300000u; a < 0x00310000u; a += 8) {
        u32 x = 0;
        mem.read32(a + 4, x);
        r_set |= (x & ppc::mmu::PTE1_R) != 0;
    }
    CHECK(!r_set);
    // Real mode: the probe is identity.
    cpu.state().msr = 0;
    REQUIRE(cpu.probe_translate(0x12345678u, false, pa));
    CHECK(pa == 0x12345678u);
}

TEST_CASE("firmware/mmu/package_methods")
{
    std::vector<u8> ram(0x10000);
    FlatMemory mem(ram.data(), static_cast<u32>(ram.size()));
    forth::Forth f(mem, 0x8000, 0x4000);
    DeviceTree tree;
    const Phandle cpu = tree.add_node(tree.root(), "PowerPC,750", "0");
    Translations t;
    MemoryMap virt;
    virt.add_free(0x10000000u, 0x70000000u);
    MmuDriver mmu(tree, t, virt);
    int syncs = 0;
    mmu.attach(cpu, [](void* c) { ++*static_cast<int*>(c); return true; }, &syncs);
    mmu.set_io_test([](void*, u32 a) { return a >= 0x80000000u; }, nullptr);
    auto call = [&](const char* name) { return mmu.method(0, name, f); };

    // claim ( size align -- base ): aligned, from free virtual space.
    f.push(0x3000); f.push(0x1000);
    REQUIRE(call("claim"));
    const u32 v = f.pop();
    CHECK(v == 0x10000000u);
    // claim ( virt size 0 -- base ): exactly there.
    f.push(0x20000000u); f.push(0x1000); f.push(0);
    REQUIRE(call("claim"));
    CHECK(f.pop() == 0x20000000u);
    // map ( phys virt size mode -- ): translations and the property follow.
    f.push(0x00400000u); f.push(v); f.push(0x3000); f.push(0x10);
    REQUIRE(call("map"));
    CHECK(!f.failed());
    CHECK(syncs == 1);
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(tree.get_property(cpu, "translations", d, len));
    CHECK(len == 16u);
    CHECK(d[0] == 0x10 && d[7] == 0x00 && d[6] == 0x30 && d[9] == 0x40 && d[15] == 0x10);
    // translate ( virt -- phys mode true | false )
    f.push(v + 0x1234);
    REQUIRE(call("translate"));
    CHECK(f.pop() == 0xFFFFFFFFu);
    CHECK(f.pop() == 0x10u);
    CHECK(f.pop() == 0x00401234u);
    f.push(0x60000000u);
    REQUIRE(call("translate"));
    CHECK(f.pop() == 0u);
    // mode -1: the binding's defaults for memory and I/O.
    f.push(0x00500000u); f.push(0x30000000u); f.push(0x1000); f.push(0xFFFFFFFFu);
    REQUIRE(call("map"));
    f.push(0x80800000u); f.push(0x30001000u); f.push(0x1000); f.push(0xFFFFFFFFu);
    REQUIRE(call("map"));
    u32 phys = 0, mode = 0;
    REQUIRE(t.translate(0x30000000u, phys, mode));
    CHECK(mode == 0x0Au);                                         // G, PP=10
    REQUIRE(t.translate(0x30001000u, phys, mode));
    CHECK(mode == 0x6Au);                                         // W, I, G, PP=10
    // unmap and release.
    f.push(v); f.push(0x3000);
    REQUIRE(call("unmap"));
    CHECK(!t.translate(v, phys, mode));
    f.push(v); f.push(0x3000);
    REQUIRE(call("release"));
    CHECK(virt.is_free(v, 0x3000));
    // A failing claim is a Forth error (scripts use catch).
    f.push(0x80000000u); f.push(0x1000);
    REQUIRE(call("claim"));
    CHECK(f.failed());
    CHECK(!call("no-such-method"));

    // The memory node: physical claim/release.
    forth::Forth g(mem, 0x8000, 0x4000);
    MemoryMap phys_map;
    phys_map.add_free(0, 0x01000000u);
    MemoryDriver memdrv(phys_map);
    g.push(0x00400000u); g.push(0x2000); g.push(0);
    REQUIRE(memdrv.method(0, "claim", g));
    CHECK(g.pop() == 0x00400000u);
    CHECK(!phys_map.is_free(0x00400000u, 0x2000));
    g.push(0x1000); g.push(0x00400000u);
    REQUIRE(memdrv.method(0, "claim", g));
    CHECK(g.pop() == 0u);                                         // lowest 4 MB-aligned free area
    g.push(0x00400000u); g.push(0x2000);
    REQUIRE(memdrv.method(0, "release", g));
    CHECK(phys_map.is_free(0x00400000u, 0x2000));
}
