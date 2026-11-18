// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/guest/run_cfm.cpp — end to end for guest code that Mac OS will
// load: guest/cfm/demo.c, compiled -fPIC -shared, converted by elf2pef,
// is prepared like the Code Fragment Manager would (tests/unit/tools/
// pef_prepare.h) at an arbitrary address and called through its exported
// transition vector on the emulated 750. Its import is a hand-assembled
// routine following the CFM conventions: it records r2, r12 and its
// arguments, and clobbers what a CFM callee may (its caller's CR/LR save
// words, the parameter area, volatile registers).
//
// usage: g3_guest_cfm demo.pef demo.so
// (the .so is converted again here to check elf2pef's refusals)

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/physical_bus.h"
#include "tests/unit/cpu/ppc_asm.h"
#include "tests/unit/tools/pef_prepare.h"
#include "tools/pef/elf2pef.h"

#include <cstdio>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kRam = 1u << 20;
// Below 0x8000: the callee reaches its record with 16-bit signed displacements from r0 = 0.
constexpr u32 kCalleeTv = 0x6000, kCalleeCode = 0x6100, kRecord = 0x7000, kReturn = 0x6F00;
constexpr u32 kCalleeToc = 0x00ABC000;
constexpr u32 kStack = 0xF0000;
constexpr u32 kStopWord = 0x00004F99u;

bool stop_trap(void* ctx, u32 insn, ppc::CpuState&, u32&)
{
    if (insn != kStopWord)
        return false;
    *static_cast<bool*>(ctx) = true;
    return true;
}

int expected(int a, int b, int calls)
{
    const int table[4] = {10, 20, 30, 40};
    const int r = a + b;  // the callee's result
    return r + a + b * 2 + (a + b) * 3 + (a - b) * 4 + table[calls & 3] + a * 7 + 1;
}

} // namespace

bool read_file(const char* path, std::vector<u8>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    for (int c; (c = std::fgetc(f)) != EOF;)
        out.push_back(static_cast<u8>(c));
    std::fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    std::vector<u8> pef, so;
    if (argc < 3 || !read_file(argv[1], pef) || !read_file(argv[2], so)) {
        std::fprintf(stderr, "usage: %s demo.pef demo.so\n", argv[0]);
        return 2;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* what, u32 got, u32 want) {
        if (!ok) {
            ++failures;
            std::printf("FAIL %s: got 0x%08X, expected 0x%08X\n", what, got, want);
        }
    };

    for (const u32 base : {0x40000u, 0x6A3F0u}) {
        const pef_test::Prepared p = pef_test::prepare(pef, {base}, {{"Callee", kCalleeTv}});
        if (!p.ok) {
            std::printf("FAIL prepare: %s\n", p.why.c_str());
            return 1;
        }
        std::vector<u8> ram(kRam);
        PhysicalBus bus;
        bus.map_ram(0, ram.data(), kRam);
        const std::vector<u8>& image = p.mem.at(0);
        for (size_t i = 0; i < image.size(); ++i)
            ram[base + i] = image[i];

        // The CFM-convention callee: record r2, r12, r3..r9; clobber what it may.
        using namespace asm_ppc;
        const std::vector<u32> callee = {
            stw(2, kRecord + 0, 0), stw(12, kRecord + 4, 0),
            stw(3, kRecord + 8, 0), stw(4, kRecord + 12, 0), stw(5, kRecord + 16, 0), stw(6, kRecord + 20, 0),
            stw(7, kRecord + 24, 0), stw(8, kRecord + 28, 0), stw(9, kRecord + 32, 0),
            add(3, 3, 4),                                     // result a + b
            lis(0, 0xBAD0),                                   // clobber: CR/LR save words, parameter area
            stw(0, 4, 1), stw(0, 8, 1),
            stw(0, 24, 1), stw(0, 28, 1), stw(0, 32, 1), stw(0, 36, 1),
            stw(0, 40, 1), stw(0, 44, 1), stw(0, 48, 1), stw(0, 52, 1),
            li(4, -1), li(5, -1), li(6, -1), li(7, -1), li(8, -1), li(9, -1), li(10, -1), li(11, -1),
            li(12, -1), li(2, -1),                            // r2 too: the caller restores its own
            blr(),
        };
        for (size_t i = 0; i < callee.size(); ++i)
            bus.write32(kCalleeCode + static_cast<u32>(4 * i), callee[i]);
        bus.write32(kCalleeTv, kCalleeCode);
        bus.write32(kCalleeTv + 4, kCalleeToc);
        bus.write32(kReturn, kStopWord);

        ppc::Ppc750 cpu(bus);
        bool stopped = false;
        cpu.set_hle_handler(stop_trap, &stopped);
        // Calls an export the way a CFM caller does: r2 and r12 from the vector.
        auto call = [&](const char* name, u32 a, u32 b, u32& result) {
            u32 value = 0;
            int16_t sec = 0;
            u8 cls = 0;
            if (!pef_test::find_export(pef, name, value, sec, cls) || sec != 0 || cls != g3::pef::kTVectorSymbol)
                return false;
            const u32 tv = base + value;
            u32 code = 0, toc = 0;
            bus.read32(tv, code);
            bus.read32(tv + 4, toc);
            if (toc != base)                                  // our vectors: TOC = the section
                return false;
            cpu.reset();
            ppc::CpuState& s = cpu.state();
            s.msr = ppc::msr::FP;
            s.pc = code;
            s.gpr[1] = kStack;
            s.gpr[2] = toc;
            s.gpr[12] = tv;
            s.gpr[3] = a;
            s.gpr[4] = b;
            s.lr() = kReturn;
            stopped = false;
            for (u64 n = 0; !stopped && !cpu.checkstopped() && n < 1000000; ++n)
                cpu.step();
            result = s.gpr[3];
            if (!stopped) std::printf("  (stuck at pc=0x%08X, entry 0x%08X, tv 0x%08X)\n", s.pc, code, tv);
            // Stack balanced, and r2 back as the caller set it (cfm_call restores it).
            return stopped && s.gpr[1] == kStack && s.gpr[2] == toc;
        };

        u32 calls = 0;
        for (const auto& ab : {std::pair<int, int>{5, 3}, {-100, 7}, {123456, -654}}) {
            u32 r = 0;
            const bool ok = call("demo_entry", static_cast<u32>(ab.first), static_cast<u32>(ab.second), r);
            check(ok, "demo_entry returned", ok, 1);
            ++calls;
            const u32 want = static_cast<u32>(expected(ab.first, ab.second, static_cast<int>(calls)));
            check(r == want, "demo_entry result", r, want);
            u32 rec[9] = {};
            for (u32 i = 0; i < 9; ++i)
                bus.read32(kRecord + 4 * i, rec[i]);
            check(rec[0] == kCalleeToc, "callee r2 = its TOC", rec[0], kCalleeToc);
            check(rec[1] == kCalleeTv, "callee r12 = its vector", rec[1], kCalleeTv);
            check(rec[2] == static_cast<u32>(ab.first), "callee arg 1", rec[2], static_cast<u32>(ab.first));
            check(rec[3] == static_cast<u32>(ab.second), "callee arg 2", rec[3], static_cast<u32>(ab.second));
            for (u32 i = 0; i < 5; ++i)
                check(rec[4 + i] == 3 + i, "callee args 3-7", rec[4 + i], 3 + i);
        }
        u32 n = 0;
        check(call("demo_calls", 0, 0, n) && n == calls, "demo_calls", n, calls);
        // The relocated pointer and the exported data.
        u32 tv_value = 0;
        int16_t sec = 0;
        u8 cls = 0;
        const bool found = pef_test::find_export(pef, "table", tv_value, sec, cls);
        check(found && cls == g3::pef::kDataSymbol, "table exported as data", cls, g3::pef::kDataSymbol);
        u32 t2 = 0;
        bus.read32(base + tv_value + 8, t2);
        check(t2 == 30, "table[2]", t2, 30);
        if (cpu.checkstopped())
            check(false, "checkstop", 0, 0);
    }
    // elf2pef: the same options give the same container; refusals.
    {
        using g3::pef::ElfToPefOptions;
        ElfToPefOptions opt;
        opt.exports = {{"demo_entry", true}, {"demo_calls", true}, {"table", false}};
        opt.imports = {{"imp_Callee", "TestLib", "Callee"}};
        std::string err;
        check(g3::pef::elf_to_pef(so, opt, &err) == pef, "conversion is reproducible", 0, 0);
        ElfToPefOptions no_import = opt;
        no_import.imports.clear();
        check(g3::pef::elf_to_pef(so, no_import, &err).empty() && err.find("imp_Callee") != std::string::npos,
              "undeclared import refused", 0, 0);
        ElfToPefOptions bad_export = opt;
        bad_export.exports.push_back({"nonexistent", true});
        check(g3::pef::elf_to_pef(so, bad_export, &err).empty(), "missing export refused", 0, 0);
        std::vector<u8> not_shared = so;
        not_shared[17] = 2;                                   // ET_EXEC
        check(g3::pef::elf_to_pef(not_shared, opt, &err).empty(), "executable refused", 0, 0);
        check(g3::pef::elf_to_pef(std::vector<u8>(so.begin(), so.begin() + 40), opt, &err).empty(),
              "truncated file refused", 0, 0);
        // Turn the import's R_PPC_ADDR32 into R_PPC_JMP_SLOT (a PLT call).
        std::vector<u8> plt = so;
        bool changed = false;
        for (size_t i = 0; i + 12 <= plt.size() && !changed; i += 4)
            if (plt[i + 4] == 0 && plt[i + 5] == 0 && plt[i + 7] == 1 && plt[i + 6] != 0 &&
                (plt[i] | plt[i + 1]) == 0 && plt[i + 8] == 0 && plt[i + 9] == 0 && plt[i + 10] == 0 &&
                plt[i + 11] == 0 && i > 52 && i < 0x200) {
                plt[i + 7] = 21;
                changed = true;
            }
        check(changed && g3::pef::elf_to_pef(plt, opt, &err).empty() && err.find("cfm_call") != std::string::npos,
              "PLT call refused", changed, 1);
    }
    std::printf("guest cfm: %d failures\n", failures);
    return failures ? 1 : 0;
}
