// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/cpu_selftest.cpp — see cpu_selftest.h.
//
// The random programs are the generator of tests/unit/cpu/test_block_cache.cpp
// (differential_run), written again without the standard library.

#include "platform/common/cpu_selftest.h"

#include "core/common/endian.h"
#include "core/cpu/ppc750/block_cache.h"
#include "core/cpu/ppc750/ppc750.h"
#include "core/cpu/ppc750/jit_worker.h"
#include "core/mem/flat_memory.h"

namespace g3 {

using namespace ppc;

namespace {

constexpr u32 kCodeBytes = 0x8000;

// --- PowerPC encodings (PEM chapter 8 forms) -----------------------------------------
constexpr u32 D(u32 op, u32 rt, u32 ra, u32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF); }
constexpr u32 X(u32 op, u32 rt, u32 ra, u32 rb, u32 xo, u32 rc = 0)
{
    return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
constexpr u32 M(u32 op, u32 rs, u32 ra, u32 sh, u32 mb, u32 me, u32 rc)
{
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc;
}
constexpr u32 spr_field(u32 spr) { return ((spr & 0x1F) << 5) | ((spr >> 5) & 0x1F); }
constexpr u32 mtspr(u32 spr, u32 rs) { return (31u << 26) | (rs << 21) | (spr_field(spr) << 11) | (467u << 1); }
constexpr u32 mfspr(u32 rd, u32 spr) { return (31u << 26) | (rd << 21) | (spr_field(spr) << 11) | (339u << 1); }
constexpr u32 b_rel(s32 disp, bool lk = false) { return (18u << 26) | (static_cast<u32>(disp) & 0x03FFFFFCu) | (lk ? 1u : 0u); }
constexpr u32 bc_rel(u32 bo, u32 bi, s32 disp, bool lk = false)
{
    return (16u << 26) | (bo << 21) | (bi << 16) | (static_cast<u32>(disp) & 0xFFFCu) | (lk ? 1u : 0u);
}

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    u32 next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    u32 below(u32 n) { return next() % n; }
};

const u32 kOp31[] = {0, 32, 8, 10, 11, 19, 24, 26, 28, 40, 60, 75, 83, 104, 124, 136, 138, 144,
                     200, 202, 232, 234, 235, 266, 284, 316, 339, 371, 412, 444, 459, 476, 491,
                     512, 536, 792, 824, 922, 954,
                     23, 55, 87, 119, 151, 183, 215, 247, 279, 311, 343, 375, 407, 439,
                     534, 662, 790, 918, 20, 150, 597, 725, 1014, 54, 86, 246, 278, 982, 598, 854};
const u32 kSafeSpr[] = {1, 8, 9, 26, 27, 272, 273, 274, 275, 22};

u32 random_insn(Rng& r, bool allow_mtmsr)
{
    const u32 rd = r.below(32), ra = r.below(32), rb = r.below(32);
    const u32 pick = r.below(100);
    if (pick < 22) {
        static const u32 ops[] = {7, 8, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29};
        return D(ops[r.below(14)], rd, ra, r.next() & 0xFFFF);
    }
    if (pick < 30)
        return M(r.below(2) ? 21 : (r.below(2) ? 20 : 23), rd, ra, rb, r.below(32), r.below(32), r.below(2));
    if (pick < 48) {
        const u32 xo = kOp31[r.below(sizeof kOp31 / sizeof kOp31[0])];
        if (xo == 339)
            return mfspr(rd, kSafeSpr[r.below(10)]);
        if (xo == 144)
            return (31u << 26) | (rd << 21) | (r.below(256) << 12) | (144u << 1);
        const bool mem = (xo & 3) == 3 || xo == 20 || xo == 150 || xo >= 534 || xo == 1014 || xo == 982 ||
                         xo == 54 || xo == 86 || xo == 246 || xo == 278;
        return X(31, rd, mem && r.below(4) ? 0 : ra, rb, xo, r.below(2)) | (r.below(8) == 0 ? 0x400u : 0);
    }
    if (pick < 68) {
        const u32 op = 32 + r.below(16);
        u32 d = r.below(kCodeBytes);
        if (op >= 40 && op <= 45) d &= ~1u;
        if (op < 40 || op == 46 || op == 47) d &= ~3u;
        return D(op, op >= 46 ? 24 + r.below(8) : rd, r.below(5) ? 0 : ra, d);
    }
    if (pick < 76) {
        const s32 disp = (static_cast<s32>(r.below(129)) - 64) * 4;
        if (r.below(2))
            return b_rel(disp, r.below(4) == 0);
        static const u32 bos[] = {4, 12, 16, 18, 20, 0, 2, 8, 10};
        return bc_rel(bos[r.below(9)], r.below(32), disp, r.below(4) == 0);
    }
    if (pick < 79)
        return r.below(2) ? X(19, r.below(2) ? 20 : 12, r.below(32), 0, 16, r.below(3) == 0)
                          : X(19, r.below(2) ? 20 : 12, r.below(32), 0, 528, r.below(3) == 0);
    if (pick < 85) {
        static const u32 xos[] = {33, 129, 193, 225, 257, 289, 417, 449};
        return r.below(4) ? X(19, rd, ra, rb, xos[r.below(8)]) : X(19, r.below(8) << 2, r.below(8) << 2, 0, 0);
    }
    if (pick < 87)
        return mtspr(kSafeSpr[r.below(10)], rd);
    if (pick < 88)
        return (17u << 26) | 2u;                              // sc
    if (pick < 89)
        return D(3, r.below(32), ra, r.next() & 0xFFFF);      // twi
    if (pick < 90)
        return allow_mtmsr ? X(31, rd, 0, 0, 146) : X(19, 0, 0, 0, 150);
    if (pick < 91)
        return r.below(3) ? X(19, 0, 0, 0, 150) : X(19, 0, 0, 0, 50);  // isync, rfi
    if (pick < 93)
        return D(48 + r.below(8), rd, 0, r.below(kCodeBytes) & ~7u);
    if (pick < 95)
        return 0;
    return r.next();
}

void set_bats(CpuState& s)
{
    // BAT 0: EA 0..1 MB = PA, read/write; BAT 1: EA 0x10000000.. -> PA 0.
    s.spr[spr::IBAT0U] = s.spr[spr::DBAT0U] = 0x0000001Fu;
    s.spr[spr::IBAT0U + 1] = s.spr[spr::DBAT0U + 1] = 0x00000002u;
    s.spr[spr::IBAT0U + 2] = s.spr[spr::DBAT0U + 2] = 0x1000001Fu;
    s.spr[spr::IBAT0U + 3] = s.spr[spr::DBAT0U + 3] = 0x00000002u;
}

void setup_state(Ppc750& cpu, Rng& r, bool translate)
{
    CpuState& s = cpu.state();
    for (u32 i = 0; i < 32; ++i)
        s.gpr[i] = r.below(3) ? r.below(kCodeBytes) & ~3u : r.next();
    s.lr() = r.below(kCodeBytes) & ~3u;
    s.ctr() = r.below(64);
    s.pc = 0x100 + 4 * r.below(256);
    s.msr = msr::ME;
    if (translate) {
        set_bats(s);
        s.msr |= msr::IR | msr::DR;
        cpu.flush_tlb();
    }
}

void copy_text(char* out, u32 cap, const char* a, const char* b = "")
{
    u32 n = 0;
    for (const char* p = a; *p && n + 1 < cap; ++p) out[n++] = *p;
    for (const char* p = b; *p && n + 1 < cap; ++p) out[n++] = *p;
    if (cap)
        out[n] = 0;
}

// Name of the first state that differs, or nullptr.
const char* differs(Ppc750& a, Ppc750& b, const u8* ram_a, const u8* ram_b)
{
    const CpuState& x = a.state();
    const CpuState& y = b.state();
    for (u32 i = 0; i < 32; ++i)
        if (x.gpr[i] != y.gpr[i]) return "a GPR";
    for (u32 i = 0; i < 32; ++i)
        if (x.fpr[i] != y.fpr[i]) return "an FPR";
    if (x.cr != y.cr) return "CR";
    if (x.fpscr != y.fpscr) return "FPSCR";
    if (x.msr != y.msr) return "MSR";
    if (x.pc != y.pc) return "the PC";
    for (u32 i = 0; i < 16; ++i)
        if (x.sr[i] != y.sr[i]) return "an SR";
    for (u32 i = 0; i < spr::kCount; ++i)
        if (x.spr[i] != y.spr[i]) return "an SPR";
    if (x.tb != y.tb) return "the time base";
    if (x.reservation_valid != y.reservation_valid ||
        (x.reservation_valid && x.reservation_addr != y.reservation_addr)) return "the reservation";
    if (a.instructions_executed() != b.instructions_executed()) return "the instruction count";
    if (a.checkstopped() != b.checkstopped()) return "checkstop";
    for (u32 i = 0; i < CpuSelfTest::kRam; ++i)
        if (ram_a[i] != ram_b[i]) return "memory";
    return nullptr;
}

struct Machines {
    u8* ram_a;
    u8* ram_b;
    FlatMemory mem_a, mem_b;
    Ppc750 interp, fast;
    BlockCache cache;
    Machines(u8* work) : ram_a(work), ram_b(work + CpuSelfTest::kRam),
                         mem_a(ram_a, CpuSelfTest::kRam), mem_b(ram_b, CpuSelfTest::kRam), interp(mem_a), fast(mem_b) {}
};

// One random program on both CPUs; nullptr if they agree throughout.
const char* differential(const CpuSelfTest::Setup& setup, u32 seed, bool translate, CpuSelfTest::Result& res)
{
    Machines* m = new Machines(setup.work);
    const char* bad = nullptr;
    if (!m->cache.init(setup.work + 2 * CpuSelfTest::kRam, CpuSelfTest::kArena, CpuSelfTest::kRam)) {
        delete m;
        return "no block cache";
    }
    m->fast.set_block_cache(&m->cache);
    if (setup.code && m->fast.set_jit(setup.code, setup.code_words, setup.runner, setup.runner_context))
        res.jit = true;
    JitWorker* worker = nullptr;
    if (res.jit && setup.worker_attach) {
        worker = new JitWorker(m->fast);
        m->fast.set_jit_worker(worker);
        setup.worker_attach(setup.worker_context, worker);
        res.background = true;
    }
    Rng r(seed);
    for (u32 a = 0; a < kCodeBytes; a += 4)
        store_be32(m->ram_a + a, random_insn(r, translate));
    for (u32 a = kCodeBytes; a < CpuSelfTest::kRam; ++a)
        m->ram_a[a] = 0;
    for (u32 a = 0; a < CpuSelfTest::kRam; ++a)
        m->ram_b[a] = m->ram_a[a];
    Rng s1(seed + 1000), s2(seed + 1000);
    setup_state(m->interp, s1, translate);
    setup_state(m->fast, s2, translate);
    Rng ev(seed + 5000);
    bool irq = false;
    for (u32 slice = 0; slice < 400 && !bad; ++slice) {
        const u32 n = 1 + ev.below(ev.below(4) ? 64 : 2000);
        m->interp.run(n);
        m->fast.run(n);
        bad = differs(m->interp, m->fast, m->ram_a, m->ram_b);
        if (bad || m->interp.checkstopped())
            break;
        if (ev.below(8) == 0)
            irq = !irq;
        const u32 ticks = ev.below(16) == 0 ? ev.below(40) : 0;
        Ppc750* both[2] = {&m->interp, &m->fast};
        for (Ppc750* c : both) {
            c->set_external_interrupt(irq);
            if (ticks)
                c->advance_timebase(ticks);
            if (slice % 50 == 49)
                c->state().msr |= msr::EE | msr::ME;
        }
    }
    res.instructions += m->interp.instructions_executed();
    res.jit_block_runs += m->fast.jit_stats().runs;
    res.installed += m->fast.jit_stats().installed;
    if (worker) {
        setup.worker_attach(setup.worker_context, nullptr);  // returns once no longer polled
        delete worker;
    }
    delete m;
    return bad;
}

// The benchmark: a loop over 4 KB of data, translated through BAT 1
// (EA 0x10000000 -> PA 0), with a call per iteration.
void load_benchmark(u8* ram)
{
    const u32 base = 0x4000;   // code
    const u32 prog[] = {
        D(15, 4, 0, 0x1001),                 // 4000 lis r4,0x1001     data at EA 0x10010000 (PA 0x10000)
        D(14, 5, 0, 1024),                   // 4004 li r5,1024
        mtspr(9, 5),                         // 4008 mtctr r5
        D(32, 6, 4, 0),                      // 400C lwz r6,0(r4)
        X(31, 3, 3, 6, 266),                 // 4010 add r3,r3,r6
        M(21, 3, 7, 3, 0, 28, 0),            // 4014 rlwinm r7,r3,3,0,28
        X(31, 3, 3, 7, 316),                 // 4018 xor r3,r3,r7
        D(36, 3, 4, 0x2000),                 // 401C stw r3,0x2000(r4)
        D(14, 4, 4, 4),                      // 4020 addi r4,r4,4
        D(11, 0, 6, 0),                      // 4024 cmpwi r6,0
        b_rel(0x4100 - 0x4028, true),        // 4028 bl 0x4100
        bc_rel(16, 0, 0x400C - 0x402C),      // 402C bdnz 0x400C
        b_rel(0x4000 - 0x4030),              // 4030 b 0x4000
    };
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        store_be32(ram + base + 4 * i, prog[i]);
    store_be32(ram + 0x4100, D(14, 8, 8, 1));            // addi r8,r8,1
    store_be32(ram + 0x4104, X(19, 20, 0, 0, 16));       // blr
    for (u32 i = 0; i < 4096; i += 4)
        store_be32(ram + 0x10000 + i, i * 2654435761u);
}

u32 bench(const CpuSelfTest::Setup& setup, int mode)  // 0 interpreter, 1 blocks, 2 generated code
{
    Machines* m = new Machines(setup.work);
    for (u32 i = 0; i < CpuSelfTest::kRam; ++i)
        m->ram_b[i] = 0;
    if (mode >= 1) {
        if (!m->cache.init(setup.work + 2 * CpuSelfTest::kRam, CpuSelfTest::kArena, CpuSelfTest::kRam)) {
            delete m;
            return 0;
        }
        m->fast.set_block_cache(&m->cache);
    }
    if (mode == 2 && !(setup.code && m->fast.set_jit(setup.code, setup.code_words, setup.runner, setup.runner_context))) {
        delete m;
        return 0;
    }
    load_benchmark(m->ram_b);
    CpuState& s = m->fast.state();
    s.pc = 0x4000;
    s.msr = msr::ME | msr::IR | msr::DR;
    set_bats(s);
    m->fast.flush_tlb();
    m->fast.run(100000);  // warm up (blocks built, TLB filled)
    const u64 t0 = setup.clock_us(setup.clock_context);
    for (u64 done = 0; done < setup.bench_instructions; done += 1000000)
        m->fast.run(1000000);
    const u64 us = setup.clock_us(setup.clock_context) - t0;
    delete m;
    return us ? static_cast<u32>(setup.bench_instructions * 100 / us) : 0;
}

void append(char* out, u32 cap, u32& n, const char* t)
{
    for (; *t && n + 1 < cap; ++t)
        out[n++] = *t;
    out[n] = 0;
}

void append_num(char* out, u32 cap, u32& n, u64 v)
{
    char tmp[24];
    u32 k = 0;
    do {
        tmp[k++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (k && n + 1 < cap)
        out[n++] = tmp[--k];
    out[n] = 0;
}

void append_mips(char* out, u32 cap, u32& n, u32 x100)
{
    append_num(out, cap, n, x100 / 100);
    append(out, cap, n, ".");
    append_num(out, cap, n, (x100 / 10) % 10);
}

static_assert(sizeof(ppc::Ppc750::JitRunner) == sizeof(CpuSelfTest::Setup::runner), "runner type");

} // namespace

CpuSelfTest::Result CpuSelfTest::run(const Setup& setup)
{
    Result res;
    if (!setup.work)
        return res;
    for (u32 p = 0; p < setup.programs; ++p) {
        const u32 seed = 7001 + p;
        const char* bad = differential(setup, seed, (p & 1) != 0, res);
        ++res.programs;
        if (bad) {
            if (!res.failed) {
                res.first_failed_program = seed;
                copy_text(res.first_failure, sizeof res.first_failure, bad, " differs");
            }
            ++res.failed;
        }
    }
    if (setup.clock_us)
        for (int mode = 0; mode < 3; ++mode)
            res.mips_x100[mode] = bench(setup, mode);
    return res;
}

void CpuSelfTest::describe(const Result& r, char* l1, u32 c1, char* l2, u32 c2)
{
    u32 n = 0;
    l1[0] = 0;
    append(l1, c1, n, r.ok() ? "CPU self-test: PASS, " : "CPU self-test: FAIL, ");
    append_num(l1, c1, n, r.programs - r.failed);
    append(l1, c1, n, "/");
    append_num(l1, c1, n, r.programs);
    append(l1, c1, n, r.jit ? (r.background ? " programs identical with generated code (compiled on another core)"
                                             : " programs identical with generated code")
                            : " programs identical (block cache only, no generated code)");
    if (r.failed) {
        append(l1, c1, n, "; first: program ");
        append_num(l1, c1, n, r.first_failed_program);
        append(l1, c1, n, ", ");
        append(l1, c1, n, r.first_failure);
    }
    n = 0;
    l2[0] = 0;
    append(l2, c2, n, "MIPS: interpreter ");
    append_mips(l2, c2, n, r.mips_x100[0]);
    append(l2, c2, n, ", block cache ");
    append_mips(l2, c2, n, r.mips_x100[1]);
    append(l2, c2, n, ", generated code ");
    if (r.mips_x100[2])
        append_mips(l2, c2, n, r.mips_x100[2]);
    else
        append(l2, c2, n, "-");
}

} // namespace g3
