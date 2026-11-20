// SPDX-License-Identifier: GPL-3.0-or-later
//
// Differential test of the soft TLB: two CPUs, one with the TLB and one
// using the uncached reference translation path, execute the same random
// stream of loads/stores/lwarx/stwcx./tlbie/mtsr under random mappings,
// protections and user/supervisor switches. After every instruction their
// registers, exception state, data memory and page table (incl. R/C bits)
// must be identical.
//
// The page table is never writable through data translations (all mapped
// pages live at 0x40000-0x7FFFF, the table at 0x10000), so the two CPUs are
// comparable at every step: the TLB may only differ from the uncached path
// when software changes PTEs without tlbie, which this stream never does.

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace g3;
using namespace g3::ppc;
using namespace asm_ppc;

namespace {

constexpr u32 kRam = 1024 * 1024;
constexpr u32 kHtab = 0x10000;       // SDR1 = 0x00010000 (64 KB, HTABMASK 0)
constexpr u32 kCode = 0x1000;

struct Rng {
    u64 s = 0x0123456789ABCDEFull;
    u32 next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    u32 below(u32 n) { return next() % n; }
};

struct Twin {
    u8* ram = new u8[kRam]();
    FlatMemory mem{ram, kRam};
    Ppc750 cpu{mem};
    ~Twin() { delete[] ram; }
};

void add_pte(u8* ram, u32 vsid, u32 ea_page, u32 pa_page, u32 pp)
{
    const u32 pidx = ea_page & 0xFFFF;
    const u32 hash = ((vsid & 0x7FFFF) ^ pidx) & 0x7FFFF;
    const u32 pteg = kHtab | ((hash & 0x3FF) << 6);
    const u32 api = pidx >> 10;
    for (u32 slot = 0; slot < 8; ++slot) {
        u8* p = ram + pteg + 8 * slot;
        if (p[0] & 0x80)
            continue;  // occupied
        const u32 w0 = 0x80000000u | (vsid << 7) | api;
        const u32 w1 = (pa_page << 12) | pp;
        store_be32(p, w0);
        store_be32(p + 4, w1);
        return;
    }
}

} // namespace

TEST_CASE("cpu/mmu/tlb_differential_random")
{
    Twin a, b;  // a: TLB on (default), b: reference
    b.cpu.set_tlb_enabled(false);
    Rng rng;

    const u32 vsids[4] = {0x010, 0x021, 0x132, 0x7A3};
    // Random mappings for EA pages 0x0..0x3F in segments 0..3.
    for (int i = 0; i < 120; ++i) {
        const u32 vsid = vsids[rng.below(4)];
        const u32 ea_page = rng.below(64);
        const u32 pa_page = 0x40 + rng.below(64);  // 0x40000-0x7FFFF
        add_pte(a.ram, vsid, ea_page, pa_page, rng.below(4));
    }
    std::memcpy(b.ram, a.ram, kRam);
    for (Twin* t : {&a, &b}) {
        auto& s = t->cpu.state();
        s.spr[spr::SDR1] = kHtab;
        for (u32 i = 0; i < 4; ++i)
            s.sr[i] = vsids[i] | (i & 1 ? mmu::SR_KP : 0) | (i & 2 ? mmu::SR_KS : 0);
        s.msr = msr::DR;
    }

    int mismatches = 0, dsis = 0;
    for (int step = 0; step < 200000 && mismatches == 0; ++step) {
        // Build one instruction.
        const u32 r_ea = 3, r_val = 4, r_dst = 5;
        u32 insn;
        // tlbie / mtsr are rare (1 in 50 each) so cached entries live long
        // enough to be hit many times.
        const u32 pick = rng.below(100);
        u32 kind;
        if (pick < 2) {
            kind = 8;   // tlbie
        } else if (pick < 4) {
            kind = 9;   // mtsr
        } else {
            const u32 k = pick % 10;
            kind = k < 8 ? k : (k == 8 ? 10 : 11);
        }
        switch (kind) {
        case 0:  insn = lwz(r_dst, rng.below(0x40) * 4, r_ea); break;
        case 1:  insn = stw(r_val, rng.below(0x40) * 4, r_ea); break;
        case 2:  insn = lbz(r_dst, rng.below(0x100), r_ea); break;
        case 3:  insn = stb(r_val, rng.below(0x100), r_ea); break;
        case 4:  insn = lhz(r_dst, rng.below(0x80) * 2, r_ea); break;
        case 5:  insn = sth(r_val, rng.below(0x80) * 2, r_ea); break;
        case 6:  insn = lwarx(r_dst, 0, r_ea); break;
        case 7:  insn = stwcx(r_val, 0, r_ea); break;
        case 8:  insn = X(31, 0, 0, r_ea, 306); break;                 // tlbie r3
        case 9:  insn = mtsr(rng.below(4), 6); break;                  // mtsr SRn, r6
        case 10: insn = lwz(r_dst, 0xFFE, r_ea); break;                // page-crossing-ish
        default: insn = stw(r_val, 0xFFE, r_ea); break;
        }
        const u32 ea = (rng.below(4) << 28) | (rng.below(64) << 12) | (rng.below(0x100) * 4);
        const u32 val = rng.next();
        const u32 new_sr = vsids[rng.below(4)] | (rng.below(4) << 29);
        const bool user = rng.below(4) == 0;

        for (Twin* t : {&a, &b}) {
            auto& s = t->cpu.state();
            store_be32(t->ram + kCode, insn);
            s.pc = kCode;
            s.msr = msr::DR | (user ? msr::PR : 0);
            s.gpr[r_ea] = ea;
            s.gpr[r_val] = val;
            s.gpr[6] = new_sr;
            t->cpu.step();
        }

        const CpuState& sa = a.cpu.state();
        const CpuState& sb = b.cpu.state();
        bool same = sa.pc == sb.pc && sa.msr == sb.msr && sa.cr == sb.cr &&
                    std::memcmp(sa.gpr, sb.gpr, sizeof sa.gpr) == 0 &&
                    std::memcmp(sa.sr, sb.sr, sizeof sa.sr) == 0 &&
                    sa.spr[spr::SRR0] == sb.spr[spr::SRR0] &&
                    sa.spr[spr::SRR1] == sb.spr[spr::SRR1] &&
                    sa.spr[spr::DAR] == sb.spr[spr::DAR] &&
                    sa.spr[spr::DSISR] == sb.spr[spr::DSISR] &&
                    sa.reservation_valid == sb.reservation_valid &&
                    std::memcmp(a.ram + kHtab, b.ram + kHtab, 0x10000) == 0 &&
                    std::memcmp(a.ram + 0x40000, b.ram + 0x40000, 0x40000) == 0;
        if (sa.pc == 0x300)
            ++dsis;
        if (!same) {
            ++mismatches;
            std::fprintf(stderr, "  step %d insn %08X ea %08X user %d: pc %08X/%08X DSISR %08X/%08X\n",
                         step, insn, ea, user, sa.pc, sb.pc, sa.spr[spr::DSISR], sb.spr[spr::DSISR]);
        }
    }
    std::fprintf(stderr, "  TLB differential: 200000 steps, %d DSIs, %d mismatches\n", dsis,
                 mismatches);
    CHECK_EQ(mismatches, 0);
    CHECK(dsis > 1000);  // the stream really exercises faults
}
