// SPDX-License-Identifier: GPL-3.0-or-later
// RTAS: the /rtas node (CHRP binding §5.2), instantiate-rtas, the call
// buffer (token nargs nret args rets; status first) and each function, with
// stand-in hooks; then the B&W machine's RTAS reached from guest code.

#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/rtas.h"
#include "core/machine/bw_firmware.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <memory>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::of;

namespace {

struct World {
    u8 nvram[64] = {};
    u64 seconds = 0;
    std::string chars;
    int power_offs = 0, reboots = 0;
    u32 cfg[4] = {0x0017106Bu, 0x02000006u, 0x00000001u, 0x12345678u};  // one function's first 16 bytes
    u32 last_bus = 99, last_devfn = 99, last_be = 0;
};

Rtas::Hooks hooks(World& w)
{
    Rtas::Hooks h;
    h.ctx = &w;
    h.nvram_size = sizeof w.nvram;
    h.nvram_read = [](void* c, u32 i) { return static_cast<World*>(c)->nvram[i]; };
    h.nvram_write = [](void* c, u32 i, u8 v) { static_cast<World*>(c)->nvram[i] = v; };
    h.seconds = [](void* c) { return static_cast<World*>(c)->seconds; };
    h.set_seconds = [](void* c, u64 s) { static_cast<World*>(c)->seconds = s; };
    h.put_char = [](void* c, char ch) { static_cast<World*>(c)->chars.push_back(ch); };
    h.power = [](void* c, bool reboot) { ++(reboot ? static_cast<World*>(c)->reboots : static_cast<World*>(c)->power_offs); };
    h.pci_read = [](void* c, u32 bus, u32 devfn, u32 reg) {
        auto* x = static_cast<World*>(c);
        x->last_bus = bus;
        x->last_devfn = devfn;
        return reg < 16 ? x->cfg[reg / 4] : 0xFFFFFFFFu;
    };
    h.pci_write = [](void* c, u32 bus, u32 devfn, u32 reg, u32 v, u32 be) {
        auto* x = static_cast<World*>(c);
        x->last_bus = bus;
        x->last_devfn = devfn;
        x->last_be = be;
        for (u32 k = 0; k < 4; ++k)
            if (be & (1u << k))
                x->cfg[reg / 4] = (x->cfg[reg / 4] & ~(0xFFu << (8 * k))) | (v & (0xFFu << (8 * k)));
    };
    return h;
}

struct Rig {
    std::vector<u8> ram = std::vector<u8>(0x10000);
    FlatMemory mem{ram.data(), static_cast<u32>(ram.size())};
    World w;
    Rtas rtas{hooks(w)};
    static constexpr u32 kArgs = 0x1000;
    // Calls a token; returns the result cells (status first).
    std::vector<u32> call(u32 token, std::vector<u32> args, u32 nret)
    {
        mem.write32(kArgs, token);
        mem.write32(kArgs + 4, static_cast<u32>(args.size()));
        mem.write32(kArgs + 8, nret);
        for (size_t i = 0; i < args.size(); ++i)
            mem.write32(kArgs + 12 + 4 * static_cast<u32>(i), args[i]);
        for (u32 i = 0; i < nret; ++i)
            mem.write32(kArgs + 12 + 4 * (static_cast<u32>(args.size()) + i), 0xEEEEEEEEu);
        std::vector<u32> out;
        if (!rtas.call(mem, kArgs))
            return out;
        for (u32 i = 0; i < nret; ++i) {
            u32 v = 0;
            mem.read32(kArgs + 12 + 4 * (static_cast<u32>(args.size()) + i), v);
            out.push_back(v);
        }
        return out;
    }
};
using V = std::vector<u32>;
constexpr u32 kParam = static_cast<u32>(Rtas::kParameterError);

} // namespace

TEST_CASE("firmware/rtas/calendar")
{
    u32 y, mo, d, h, mi, s;
    Rtas::to_calendar(0, y, mo, d, h, mi, s);
    CHECK(y == 1970 && mo == 1 && d == 1 && h == 0 && mi == 0 && s == 0);
    Rtas::to_calendar(978307200ull, y, mo, d, h, mi, s);         // 2001-01-01 (a known Unix time)
    CHECK(y == 2001 && mo == 1 && d == 1);
    Rtas::to_calendar(951782400ull + 86399, y, mo, d, h, mi, s); // 2000-02-29 23:59:59 (leap day)
    CHECK(y == 2000 && mo == 2 && d == 29 && h == 23 && mi == 59 && s == 59);
    Rtas::to_calendar(4102444800ull, y, mo, d, h, mi, s);        // 2100-01-01
    CHECK(y == 2100 && mo == 1 && d == 1);
    u64 t = 0;
    REQUIRE(Rtas::from_calendar(2000, 2, 29, 23, 59, 59, t));
    CHECK(t == 951782400ull + 86399);
    CHECK(!Rtas::from_calendar(1900, 2, 29, 0, 0, 0, t));        // before 1970
    CHECK(!Rtas::from_calendar(2001, 2, 29, 0, 0, 0, t));        // not a leap year
    CHECK(!Rtas::from_calendar(2100, 2, 29, 0, 0, 0, t));        // century rule
    CHECK(Rtas::from_calendar(2400, 2, 29, 0, 0, 0, t));         // 400-year rule
    CHECK(!Rtas::from_calendar(2001, 13, 1, 0, 0, 0, t));
    CHECK(!Rtas::from_calendar(2001, 4, 31, 0, 0, 0, t));
    // Round trips over many days.
    bool same = true;
    for (u64 s0 = 0; s0 < 4102444800ull && same; s0 += 86400ull * 37 + 3601) {
        Rtas::to_calendar(s0, y, mo, d, h, mi, s);
        same = Rtas::from_calendar(y, mo, d, h, mi, s, t) && t == s0;
    }
    CHECK(same);
}

TEST_CASE("firmware/rtas/node_and_instantiate")
{
    Rig r;
    DeviceTree t;
    const Phandle n = r.rtas.add_node(t, t.root());
    REQUIRE(n != kNone);
    CHECK(std::string(t.name(n)) == "rtas");
    CHECK(!t.has_property(n, "reg") && !t.has_property(n, "ranges"));   // binding §5.2
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(t.get_property(n, "rtas-size", d, len));
    CHECK(len == 4 && d[2] == 0x10);
    for (const char* f : {"get-time-of-day", "set-time-of-day", "nvram-fetch", "nvram-store", "display-character",
                          "power-off", "system-reboot", "read-pci-config", "write-pci-config"})
        CHECK(t.has_property(n, f));
    CHECK(!t.has_property(n, "event-scan"));                             // not implemented: no property
    // instantiate-rtas ( base -- entry ) writes the trap word there.
    forth::Forth f(r.mem, 0x8000, 0x4000);
    r.rtas.set_trap_word(0x00004F10u);
    f.push(0x3000);
    REQUIRE(r.rtas.method(0, "instantiate-rtas", f));
    CHECK(f.pop() == 0x3000u);
    u32 word = 0;
    r.mem.read32(0x3000, word);
    CHECK(word == 0x00004F10u);
    CHECK(r.rtas.base() == 0x3000u);
    f.push(0x3002);                                                     // misaligned
    REQUIRE(r.rtas.method(0, "instantiate-rtas", f));
    CHECK(f.failed());
    CHECK(!r.rtas.method(0, "frob", f));
}

TEST_CASE("firmware/rtas/functions")
{
    Rig r;
    // get-time-of-day ( -- status year month day hour min sec nsec )
    r.w.seconds = 978307200ull + 3600 * 13 + 60 * 14 + 15;               // 2001-01-01 13:14:15
    CHECK(r.call(Rtas::kGetTimeOfDay, {}, 8) == V({0, 2001, 1, 1, 13, 14, 15, 0}));
    CHECK(r.call(Rtas::kGetTimeOfDay, {}, 7)[0] == kParam);
    // set-time-of-day
    CHECK(r.call(Rtas::kSetTimeOfDay, {2002, 3, 4, 5, 6, 7, 0}, 1) == V({0}));
    u64 t = 0;
    Rtas::from_calendar(2002, 3, 4, 5, 6, 7, t);
    CHECK(r.w.seconds == t);
    CHECK(r.call(Rtas::kSetTimeOfDay, {2002, 2, 30, 0, 0, 0, 0}, 1) == V({kParam}));
    // nvram-store / nvram-fetch ( index buffer length -- status actual )
    const char msg[] = "hello";
    for (u32 i = 0; i < 5; ++i) r.ram[0x2000 + i] = static_cast<u8>(msg[i]);
    CHECK(r.call(Rtas::kNvramStore, {10, 0x2000, 5}, 2) == V({0, 5}));
    CHECK(r.w.nvram[10] == 'h' && r.w.nvram[14] == 'o');
    CHECK(r.call(Rtas::kNvramFetch, {10, 0x2100, 5}, 2) == V({0, 5}));
    CHECK(std::string(reinterpret_cast<char*>(&r.ram[0x2100]), 5) == "hello");
    CHECK(r.call(Rtas::kNvramFetch, {60, 0x2200, 10}, 2) == V({0, 4}));      // clipped at the end
    CHECK(r.call(Rtas::kNvramFetch, {64, 0x2200, 1}, 2)[0] == kParam);       // out of range
    // display-character, power-off, system-reboot
    CHECK(r.call(Rtas::kDisplayCharacter, {'A'}, 1) == V({0}));
    CHECK(r.w.chars == "A");
    CHECK(r.call(Rtas::kPowerOff, {0xFFFFFFFFu, 0xFFFFFFFFu}, 1) == V({0}));
    CHECK(r.w.power_offs == 1);
    CHECK(r.call(Rtas::kSystemReboot, {}, 1) == V({0}));
    CHECK(r.w.reboots == 1);
    // read-pci-config ( addr size -- status value ), addr = bus << 16 | devfn << 8 | reg
    const u32 at = (1u << 16) | (0x28u << 8);
    CHECK(r.call(Rtas::kReadPciConfig, {at | 0, 4}, 2) == V({0, 0x0017106Bu}));
    CHECK(r.w.last_bus == 1u && r.w.last_devfn == 0x28u);
    CHECK(r.call(Rtas::kReadPciConfig, {at | 0, 2}, 2) == V({0, 0x106Bu}));   // vendor ID
    CHECK(r.call(Rtas::kReadPciConfig, {at | 2, 2}, 2) == V({0, 0x0017u}));   // device ID
    CHECK(r.call(Rtas::kReadPciConfig, {at | 7, 1}, 2) == V({0, 0x02u}));     // top byte of register 4
    CHECK(r.call(Rtas::kReadPciConfig, {at | 8, 1}, 2) == V({0, 0x01u}));     // bottom byte of register 8
    CHECK(r.call(Rtas::kReadPciConfig, {at | 1, 2}, 2)[0] == kParam);         // misaligned
    CHECK(r.call(Rtas::kReadPciConfig, {at | 0, 3}, 2)[0] == kParam);         // bad size
    // write-pci-config ( addr size value -- status )
    CHECK(r.call(Rtas::kWritePciConfig, {at | 4, 2, 0x0006}, 1) == V({0}));
    CHECK(r.w.last_be == 0x3u);
    CHECK(r.w.cfg[1] == 0x02000006u);
    CHECK(r.call(Rtas::kWritePciConfig, {at | 0xF, 1, 0xAB}, 1) == V({0}));
    CHECK(r.w.last_be == 0x8u);
    CHECK(r.w.cfg[3] == 0xAB345678u);
    // An unknown token: hardware error; no results beyond the status.
    CHECK(r.call(99, {}, 2) == V({static_cast<u32>(Rtas::kHardwareError), 0xEEEEEEEEu}));
    CHECK(r.rtas.calls() > 10);
}

TEST_CASE("firmware/rtas/machine_rtas_from_guest_code")
{
    using namespace asm_ppc;
    constexpr u32 kRam = 16u << 20;
    std::vector<u8> ram(kRam);
    YosemiteConfig cfg;
    cfg.ram = ram.data();
    cfg.ram_size = kRam;
    YosemiteMachine m(cfg);
    BwFirmware::Config fc;
    fc.rtc_seconds = 978307200ull;
    BwFirmware fw(m, fc);
    REQUIRE(fw.ok());
    // As a client would: open /rtas, instantiate at 0x8000.
    REQUIRE(fw.command("\" /rtas\" open-dev constant rt  8000 \" instantiate-rtas\" rt $call-method constant entry"));
    REQUIRE(fw.command("entry"));
    CHECK(fw.forth().pop() == 0x8000u);
    // Guest code: get-time-of-day, then read mac-io's vendor ID (bus 1, device 5).
    auto w32 = [&](u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) ram[a + k] = static_cast<u8>(v >> (24 - 8 * k)); };
    auto r32 = [&](u32 a) { u32 v = 0; for (u32 k = 0; k < 4; ++k) v = (v << 8) | ram[a + k]; return v; };
    w32(0x9000, Rtas::kGetTimeOfDay); w32(0x9004, 0); w32(0x9008, 8);
    w32(0x9100, Rtas::kReadPciConfig); w32(0x9104, 2); w32(0x9108, 2);
    w32(0x910C, (1u << 16) | ((5u << 3) << 8) | 0); w32(0x9110, 2);
    const u32 prog[] = {
        lis(9, 0), ori(9, 9, 0x8000), mtspr(ppc::spr::CTR, 9),
        lis(3, 0), ori(3, 3, 0x9000), bctr() | 1,
        lis(3, 0), ori(3, 3, 0x9100), bctr() | 1,
        b(0),
    };
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        w32(0x7000 + 4 * i, prog[i]);
    ppc::CpuState& s = m.cpu().state();
    s.msr = ppc::msr::ME;                                 // RTAS is called with translation off
    s.pc = 0x7000;
    m.cpu().run(40);
    CHECK(r32(0x900C) == 0u);                             // status
    CHECK(r32(0x9010) == 2001u && r32(0x9014) == 1u && r32(0x9018) == 1u);
    CHECK(r32(0x9114) == 0u);
    CHECK(r32(0x9118) == 0x106Bu);                        // Apple's vendor ID, through Grackle and the bridge
    CHECK(s.pc == 0x7000u + 4 * (sizeof prog / 4 - 1));
}

TEST_CASE("firmware/rtas/unexpected_exceptions_are_reported")
{
    using namespace asm_ppc;
    constexpr u32 kRam = 16u << 20;
    std::vector<u8> ram(kRam);
    YosemiteConfig cfg;
    cfg.ram = ram.data();
    cfg.ram_size = kRam;
    YosemiteMachine m(cfg);
    BwFirmware fw(m, BwFirmware::Config{});
    REQUIRE(fw.ok());
    // An illegal instruction with vectors at 0: the firmware's vector trap reports it.
    ram[0x7000] = 0x7C; ram[0x7001] = 0x00; ram[0x7002] = 0x02; ram[0x7003] = 0xA6;   // mfspr r0, 0 (MQ: not on the 750)
    ppc::CpuState& s = m.cpu().state();
    s.msr = ppc::msr::ME;
    s.pc = 0x7000;
    m.cpu().run(10);
    CHECK_EQ(fw.firmware().exception_vector(), 0x700u);
    CHECK_EQ(fw.firmware().exception_srr0(), 0x7000u);
    CHECK(fw.client_exited());
}
