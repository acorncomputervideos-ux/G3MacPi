// SPDX-License-Identifier: GPL-3.0-or-later
// The B&W firmware in Virtual-Mode (PowerPC binding §4.2): /chosen mmu and
// memory, the translations property, Forth and the client interface
// reading and writing through the firmware's translations.

#include "core/machine/bw_firmware.h"
#include "tests/framework/g3test.h"

#include <memory>
#include <string>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kRam = 32u << 20;

struct Rig {
    std::vector<u8> ram = std::vector<u8>(kRam);
    std::unique_ptr<YosemiteMachine> m;
    std::unique_ptr<BwFirmware> fw;
    Rig()
    {
        YosemiteConfig cfg;
        cfg.ram = ram.data();
        cfg.ram_size = kRam;
        m = std::make_unique<YosemiteMachine>(cfg);
        fw = std::make_unique<BwFirmware>(*m, BwFirmware::Config{});
    }
    u32 phys32(u32 a) const { u32 v = 0; for (u32 k = 0; k < 4; ++k) v = (v << 8) | ram[a + k]; return v; }
    void put_phys(u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) ram[a + k] = static_cast<u8>(v >> (24 - 8 * k)); }
    u32 prop_cell(of::Phandle h, const char* name, u32 i = 0)
    {
        const u8* d = nullptr;
        u32 len = 0;
        if (!fw->tree().get_property(h, name, d, len) || len < 4 * (i + 1))
            return 0xDEADDEADu;
        d += 4 * i;
        return (static_cast<u32>(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
    }
    u32 run(const std::string& forth)
    {
        if (!fw->command(forth.c_str()))
            return 0xBAD0BAD0u;
        return fw->forth().pop();
    }
};

const char* kSetup =
    "\" mmu\" \" /chosen\" find-package drop get-package-property drop decode-int constant xmmu 2drop "
    "\" memory\" \" /chosen\" find-package drop get-package-property drop decode-int constant xmem 2drop";

} // namespace

TEST_CASE("machine/bw-virtual/power_on")
{
    Rig r;
    REQUIRE(r.fw->ok());
    const auto& n = r.fw->nodes();
    // /chosen names instances of the CPU node (mmu) and of /memory.
    const u32 mmu = r.prop_cell(n.chosen, "mmu"), mem = r.prop_cell(n.chosen, "memory");
    CHECK(r.fw->instances().package(mmu) == n.cpu);
    CHECK(r.fw->instances().package(mem) == n.memory);
    // translations (virt size phys mode): RAM 1:1 first.
    CHECK_EQ(r.prop_cell(n.cpu, "translations", 0), 0u);
    CHECK_EQ(r.prop_cell(n.cpu, "translations", 1), kRam);
    CHECK_EQ(r.prop_cell(n.cpu, "translations", 2), 0u);
    // mac-io is translated 1:1, cache-inhibited and guarded.
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(r.fw->tree().get_property(n.cpu, "translations", d, len));
    bool macio = false;
    for (u32 i = 0; i + 16 <= len; i += 16)
        macio |= r.prop_cell(n.cpu, "translations", i / 4) == 0x80800000u &&
                 r.prop_cell(n.cpu, "translations", i / 4 + 2) == 0x80800000u &&
                 (r.prop_cell(n.cpu, "translations", i / 4 + 3) & 0x28) == 0x28;
    CHECK(macio);
    REQUIRE(r.fw->tree().get_property(n.options, "real-mode?", d, len));
    CHECK(std::string(reinterpret_cast<const char*>(d)) == "false");
    CHECK(r.fw->tree().find("/rom") != of::kNone);
    // The CPU state is in Virtual-Mode already, with a page table in firmware memory.
    const auto& s = r.m->cpu().state();
    CHECK((s.msr & (ppc::msr::IR | ppc::msr::DR)) == (ppc::msr::IR | ppc::msr::DR));
    CHECK_EQ(s.spr[ppc::spr::SDR1] & 0xFFFF0000u, r.fw->layout().htab);
}

TEST_CASE("machine/bw-virtual/forth_maps_and_writes_through_a_virtual_address")
{
    Rig r;
    REQUIRE(r.fw->ok());
    REQUIRE(r.fw->command(kSetup));
    // Physical pages (top-down) and virtual pages (above RAM), then map.
    const u32 p = r.run("2000 1000 \" claim\" xmem $call-method dup constant p");
    const u32 v = r.run("2000 1000 \" claim\" xmmu $call-method dup constant v");
    CHECK(p < r.fw->layout().base && p > kRam / 2);
    CHECK(v >= kRam && v < 0x80000000u);
    REQUIRE(r.fw->command("p v 2000 10 \" map\" xmmu $call-method"));
    REQUIRE(r.fw->command("12345678 v 1004 + !  v 1004 + @ 1+ v 8 + !"));
    CHECK_EQ(r.phys32(p + 0x1004), 0x12345678u);           // landed in the physical page
    CHECK_EQ(r.phys32(p + 8), 0x12345679u);
    CHECK_EQ(r.phys32(v & 0x00FFFFFFu), 0u);                // and nowhere else
    // translate ( virt -- phys mode true )
    CHECK_EQ(r.run("v 1234 + \" translate\" xmmu $call-method"), 0xFFFFFFFFu);
    CHECK_EQ(r.fw->forth().pop(), 0x10u);
    CHECK_EQ(r.fw->forth().pop(), p + 0x1234);
    // The CPU sees the same translation.
    u32 pa = 0;
    REQUIRE(r.m->cpu().probe_translate(v + 0x10, false, pa));
    CHECK_EQ(pa, p + 0x10);
    // The translations property lists it.
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(r.fw->tree().get_property(r.fw->nodes().cpu, "translations", d, len));
    bool listed = false;
    for (u32 i = 0; i + 16 <= len; i += 16)
        listed |= r.prop_cell(r.fw->nodes().cpu, "translations", i / 4) == v &&
                  r.prop_cell(r.fw->nodes().cpu, "translations", i / 4 + 1) == 0x2000u &&
                  r.prop_cell(r.fw->nodes().cpu, "translations", i / 4 + 2) == p;
    CHECK(listed);
    // unmap: the address no longer works, for Forth or the CPU.
    REQUIRE(r.fw->command("v 2000 \" unmap\" xmmu $call-method"));
    CHECK(!r.fw->command("v @"));
    CHECK(!r.m->cpu().probe_translate(v + 0x10, false, pa));
    // A non-1:1 mapping inside the RAM window (where a BAT maps 1:1) is refused
    // rather than silently hidden.
    CHECK(!r.fw->command("p 100000 1000 10 \" map\" xmmu $call-method"));
}

TEST_CASE("machine/bw-virtual/client_interface_takes_virtual_addresses")
{
    Rig r;
    REQUIRE(r.fw->ok());
    REQUIRE(r.fw->command(kSetup));
    const u32 p = r.run("1000 1000 \" claim\" xmem $call-method dup constant p");
    const u32 v = r.run("1000 1000 \" claim\" xmmu $call-method dup constant v");
    REQUIRE(r.fw->command("p v 1000 10 \" map\" xmmu $call-method"));
    // An argument array for finddevice("/chosen"), built physically at p,
    // whose pointers are virtual: the service name and the path.
    const char* name = "finddevice";
    const char* path = "/chosen";
    for (u32 i = 0; name[i] || i == 0; ++i) r.ram[p + 0x100 + i] = static_cast<u8>(name[i]);
    r.ram[p + 0x100 + 10] = 0;
    for (u32 i = 0; i < 8; ++i) r.ram[p + 0x200 + i] = static_cast<u8>(path[i]);
    r.put_phys(p + 0, v + 0x100);
    r.put_phys(p + 4, 1);
    r.put_phys(p + 8, 1);
    r.put_phys(p + 12, v + 0x200);
    r.put_phys(p + 16, 0xFFFFFFFFu);
    CHECK_EQ(r.fw->client().call(v), 0u);
    CHECK_EQ(r.phys32(p + 16), r.fw->nodes().chosen);
}

TEST_CASE("machine/bw-virtual/physical_claims_come_from_the_top")
{
    Rig r;
    REQUIRE(r.fw->ok());
    REQUIRE(r.fw->command(kSetup));
    // An aligned claim must not take low memory, where programs such as
    // Apple's Mac OS ROM loader are placed at fixed addresses.
    const u32 big = r.run("200000 400000 \" claim\" xmem $call-method");
    CHECK(big >= 0x01000000u);
    CHECK_EQ(big % 0x400000u, 0u);
    // The exception vectors are never handed out.
    CHECK(!r.fw->command("0 1000 0 \" claim\" xmem $call-method"));
}
