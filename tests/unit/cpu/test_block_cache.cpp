// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoded-block cache (core/cpu/ppc750/block_cache.h) against the
// interpreter. Two CPUs run the same program: one interprets, one uses the
// block cache. After every slice of a random length their registers,
// instruction counts and all of memory must be identical.
//
// The programs are random instruction mixes over the whole low 32 KB, the
// exception vectors included: arithmetic, loads and stores (many of them
// into the code itself), branches, CR operations, system calls, traps,
// MSR and SPR writes, illegal words. Between slices both CPUs get the same
// external-interrupt changes and time-base advances, so interrupts arrive
// in the middle of blocks. Some runs have address translation on (BATs).

#include "core/cpu/ppc750/block_cache.h"
#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"
#include "tools/a64sim/a64_sim.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace g3;
using namespace g3::ppc;
using namespace asm_ppc;

namespace {

constexpr u32 kRam = 1024 * 1024;
constexpr u32 kCodeBytes = 0x8000;

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

// Every test runs twice: "cpu/blocks/..." with the block cache, and
// "cpu/jit/..." with generated AArch64 code for the blocks as well (run on
// the A64 simulator here; natively on an AArch64 host).
bool g_jit = false;
u32 g_jit_words = 1u << 20;

struct Machine {
    std::vector<u8> ram = std::vector<u8>(kRam);
    FlatMemory mem{ram.data(), kRam};
    Ppc750 cpu{mem};
    std::vector<u8> arena;
    BlockCache cache;
    std::vector<u32> code;
    const void* range[2] = {};
    explicit Machine(u32 arena_bytes)
    {
        if (arena_bytes) {
            arena.resize(arena_bytes);
            REQUIRE(cache.init(arena.data(), arena_bytes, kRam));
            cpu.set_block_cache(&cache);
            if (g_jit) {
                code.resize(g_jit_words);
                range[0] = code.data();
                range[1] = code.data() + code.size();
#if defined(__aarch64__)
                REQUIRE(cpu.set_jit(code.data(), g_jit_words, nullptr, nullptr));
#else
                REQUIRE(cpu.set_jit(code.data(), g_jit_words, a64::simulate_jit_block, range));
#endif
            }
        }
    }
};

const u32 kOp31[] = {0, 32, 8, 10, 11, 19, 24, 26, 28, 40, 60, 75, 83, 104, 124, 136, 138, 144,
                     200, 202, 232, 234, 235, 266, 284, 316, 339, 371, 412, 444, 459, 476, 491,
                     512, 536, 792, 824, 922, 954,
                     // loads/stores (indexed)
                     23, 55, 87, 119, 151, 183, 215, 247, 279, 311, 343, 375, 407, 439,
                     534, 662, 790, 918, 20, 150, 597, 725, 1014, 54, 86, 246, 278, 982, 598, 854};
const u32 kSafeSpr[] = {1, 8, 9, 26, 27, 272, 273, 274, 275, 22};

u32 random_insn(Rng& r, bool allow_mtmsr)
{
    const u32 rd = r.below(32), ra = r.below(32), rb = r.below(32);
    const u32 pick = r.below(100);
    if (pick < 22) {  // D-form arithmetic, compare, logical
        static const u32 ops[] = {7, 8, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29};
        return D(ops[r.below(14)], rd, ra, r.next() & 0xFFFF);
    }
    if (pick < 30)  // rotates
        return M(r.below(2) ? 21 : (r.below(2) ? 20 : 23), rd, ra, rb, r.below(32), r.below(32), r.below(2));
    if (pick < 48) {  // op31, random OE/Rc
        const u32 xo = kOp31[r.below(sizeof kOp31 / sizeof kOp31[0])];
        if (xo == 339)
            return mfspr(rd, kSafeSpr[r.below(10)]);
        if (xo == 144)
            return mtcrf(r.below(256), rd);
        // Indexed accesses: base 0 (absolute) most of the time, so they land
        // in the program and its data.
        const bool mem = (xo & 3) == 3 || xo == 20 || xo == 150 || xo >= 534 || xo == 1014 || xo == 982 ||
                         xo == 54 || xo == 86 || xo == 246 || xo == 278;
        return X(31, rd, mem && r.below(4) ? 0 : ra, rb, xo, r.below(2)) | (r.below(8) == 0 ? 0x400u : 0);
    }
    if (pick < 68) {  // D-form loads and stores, absolute into the low 32 KB (code included)
        const u32 op = 32 + r.below(16);
        u32 d = r.below(kCodeBytes);
        if (op == 40 || op == 41 || op == 42 || op == 43 || op == 44 || op == 45) d &= ~1u;
        if (op < 40 || op == 46 || op == 47) d &= ~3u;
        return D(op, op >= 46 ? 24 + r.below(8) : rd, r.below(5) ? 0 : ra, d);
    }
    if (pick < 76) {  // relative branches nearby (sometimes linking)
        const int32_t disp = (static_cast<int32_t>(r.below(129)) - 64) * 4;
        if (r.below(2))
            return b(disp, false, r.below(4) == 0);
        static const u32 bos[] = {4, 12, 16, 18, 20, 0, 2, 8, 10};
        return bc(bos[r.below(9)], r.below(32), disp, false, r.below(4) == 0);
    }
    if (pick < 79) {  // bclr / bcctr: LR and CTR mostly hold small addresses here
        return r.below(2) ? bclr(r.below(2) ? 20 : 12, r.below(32), r.below(3) == 0)
                          : X(19, r.below(2) ? 20 : 12, r.below(32), 0, 528, r.below(3) == 0);
    }
    if (pick < 85) {  // CR logical, mcrf
        static const u32 xos[] = {33, 129, 193, 225, 257, 289, 417, 449};
        return r.below(4) ? crop(xos[r.below(8)], rd, ra, rb) : mcrf(r.below(8), r.below(8));
    }
    if (pick < 87)
        return mtspr(kSafeSpr[r.below(10)], rd);
    if (pick < 88)
        return sc();
    if (pick < 89)
        return twi(r.below(32), ra, r.next() & 0xFFFF);
    if (pick < 90)
        return allow_mtmsr ? mtmsr(rd) : isync();
    if (pick < 91)
        return r.below(3) ? isync() : rfi();
    if (pick < 93)  // FP (unavailable unless MSR[FP]): exceptions
        return D(48 + r.below(8), rd, 0, r.below(kCodeBytes) & ~7u);
    if (pick < 95)
        return 0x00000000u;  // illegal
    return r.next();         // anything at all
}

void load_program(Machine& m, Rng& r, bool allow_mtmsr)
{
    for (u32 a = 0; a < kCodeBytes; a += 4)
        store_be32(m.ram.data() + a, random_insn(r, allow_mtmsr));
}

void setup_state(Machine& m, Rng& r, bool translate)
{
    CpuState& s = m.cpu.state();
    for (u32 i = 0; i < 32; ++i)
        s.gpr[i] = r.below(3) ? r.below(kCodeBytes) & ~3u : r.next();
    s.lr() = r.below(kCodeBytes) & ~3u;
    s.ctr() = r.below(64);
    s.pc = 0x100 + 4 * r.below(256);
    s.msr = msr::ME;
    if (translate) {
        // BAT 0 maps EA 0..1 MB to PA 0 read/write; BAT 1 maps EA 0x10000000 to PA 0.
        s.spr[spr::IBAT0U] = s.spr[spr::DBAT0U] = 0x0000001Fu;
        s.spr[spr::IBAT0U + 1] = s.spr[spr::DBAT0U + 1] = 0x00000002u;
        s.spr[spr::IBAT0U + 2] = s.spr[spr::DBAT0U + 2] = 0x1000001Fu;
        s.spr[spr::IBAT0U + 3] = s.spr[spr::DBAT0U + 3] = 0x00000002u;
        s.msr |= msr::IR | msr::DR;
        m.cpu.flush_tlb();
    }
}

bool same(Machine& a, Machine& b, const char* what, int run, u32 slice)
{
    const CpuState& x = a.cpu.state();
    const CpuState& y = b.cpu.state();
    const char* bad = nullptr;
    if (std::memcmp(x.gpr, y.gpr, sizeof x.gpr)) bad = "gpr";
    else if (std::memcmp(x.fpr, y.fpr, sizeof x.fpr)) bad = "fpr";
    else if (x.cr != y.cr) bad = "cr";
    else if (x.fpscr != y.fpscr) bad = "fpscr";
    else if (x.msr != y.msr) bad = "msr";
    else if (x.pc != y.pc) bad = "pc";
    else if (std::memcmp(x.sr, y.sr, sizeof x.sr)) bad = "sr";
    else if (std::memcmp(x.spr, y.spr, sizeof x.spr)) bad = "spr";
    else if (x.tb != y.tb) bad = "tb";
    else if (x.reservation_valid != y.reservation_valid ||
             (x.reservation_valid && x.reservation_addr != y.reservation_addr)) bad = "reservation";
    else if (a.cpu.instructions_executed() != b.cpu.instructions_executed()) bad = "instruction count";
    else if (a.cpu.checkstopped() != b.cpu.checkstopped()) bad = "checkstop";
    else if (a.ram != b.ram) bad = "memory";
    if (bad)
        std::printf("    %s run %d slice %u: %s differs (pc %08X vs %08X, count %llu vs %llu)\n", what, run,
                    slice, bad, x.pc, y.pc, static_cast<unsigned long long>(a.cpu.instructions_executed()),
                    static_cast<unsigned long long>(b.cpu.instructions_executed()));
    return bad == nullptr;
}

struct Totals {
    u64 hits = 0, built = 0, invalidations = 0, flushes = 0, executed = 0;
    u64 jit_runs = 0, jit_native = 0, jit_resets = 0;
};

// One random program on an interpreter and a block-cache CPU; false on
// the first difference.
bool differential_run(int seed, bool translate, u32 arena_bytes, Totals& t)
{
    Machine interp(0), blocks(arena_bytes);
    Rng r(static_cast<u64>(seed));
    load_program(interp, r, translate);
    std::memcpy(blocks.ram.data(), interp.ram.data(), kRam);
    Rng rs(static_cast<u64>(seed) + 1000);
    setup_state(interp, rs, translate);
    Rng rs2(static_cast<u64>(seed) + 1000);
    setup_state(blocks, rs2, translate);

    Rng ev(static_cast<u64>(seed) + 5000);
    bool irq = false;
    for (u32 slice = 0; slice < 400; ++slice) {
        const u32 n = 1 + ev.below(ev.below(4) ? 64 : 2000);
        interp.cpu.run(n);
        blocks.cpu.run(n);
        if (!same(interp, blocks, translate ? "translated" : "real", seed, slice))
            return false;
        if (interp.cpu.checkstopped())
            break;
        // Same events for both, between slices (so mid-block for the cache).
        if (ev.below(8) == 0)
            irq = !irq;
        const u32 ticks = ev.below(16) == 0 ? ev.below(40) : 0;
        for (Machine* m : {&interp, &blocks}) {
            m->cpu.set_external_interrupt(irq);
            if (ticks)
                m->cpu.advance_timebase(ticks);
            // Keep the programs alive: an occasional enable of EE/ME.
            if (slice % 50 == 49)
                m->cpu.state().msr |= msr::EE | msr::ME;
        }
    }
    const BlockCache::Stats& st = blocks.cache.stats();
    t.hits += st.hits;
    t.built += st.built;
    t.invalidations += st.invalidations;
    t.flushes += st.flushes;
    t.executed += blocks.cpu.instructions_executed();
    t.jit_runs += blocks.cpu.jit_stats().runs;
    t.jit_native += blocks.cpu.jit_stats().native_ops;
    t.jit_resets += blocks.cpu.jit_stats().resets;
    return true;
}

} // namespace

// Defines the test body once and registers it in both modes.
#define BOTH(name)                                                                        \
    static void name##_body();                                                            \
    TEST_CASE("cpu/blocks/" #name)                                                        \
    {                                                                                     \
        g_jit = false;                                                                    \
        name##_body();                                                                    \
    }                                                                                     \
    TEST_CASE("cpu/jit/" #name)                                                           \
    {                                                                                     \
        g_jit = true;                                                                     \
        name##_body();                                                                    \
        g_jit = false;                                                                    \
    }                                                                                     \
    static void name##_body()

BOTH(differential_real_mode)
{
    Totals t;
    int failed = 0;
    for (int seed = 1; seed <= 300 && failed < 3; ++seed)
        failed += !differential_run(seed, false, 4u << 20, t);
    CHECK_EQ(failed, 0);
    std::printf("    real mode: %llu instructions, %llu blocks built, %llu hits, %llu page invalidations\n",
                static_cast<unsigned long long>(t.executed), static_cast<unsigned long long>(t.built),
                static_cast<unsigned long long>(t.hits), static_cast<unsigned long long>(t.invalidations));
    // The test means something only if blocks were reused and code was rewritten.
    CHECK(t.hits > t.built);
    CHECK(t.invalidations > 100);
    if (g_jit) {
        std::printf("    jit: %llu block runs in generated code, %llu instructions compiled natively\n",
                    static_cast<unsigned long long>(t.jit_runs), static_cast<unsigned long long>(t.jit_native));
        CHECK(t.jit_runs > t.built);
        CHECK(t.jit_native > 10000);
    }
}

BOTH(differential_translated)
{
    Totals t;
    int failed = 0;
    for (int seed = 1001; seed <= 1200 && failed < 3; ++seed)
        failed += !differential_run(seed, true, 4u << 20, t);
    CHECK_EQ(failed, 0);
    CHECK(t.hits > t.built);
}

BOTH(differential_tiny_cache)
{
    // A cache with the smallest pool init() accepts: it fills and is flushed
    // over and over while running.
    Totals t;
    int failed = 0;
    const u32 arena = 4 * (kRam >> 12) * 5 + 4 * BlockCache::kTableSize + 64 * BlockCache::block_bytes(BlockCache::kMaxOps) + 64;
    for (int seed = 2001; seed <= 2100 && failed < 3; ++seed)
        failed += !differential_run(seed, seed & 1, arena, t);
    CHECK_EQ(failed, 0);
    CHECK(t.flushes > 100);
}

BOTH(self_modifying_loop)
{
    // A loop that rewrites the instruction it runs next: "addi r3,r3,1" is
    // replaced with "addi r3,r3,100" by the loop itself, in the same block.
    Machine m(4u << 20);
    u8* ram = m.ram.data();
    const u32 base = 0x2000;
    const u32 prog[] = {
        li(4, 10),               // 2000  counter
        lis(5, 0x3863),          // 2004  r5 = addi r3,r3,100 = 0x38630064
        ori(5, 5, 0x0064),       // 2008
        stw(5, 0x2014, 0),       // 200C  overwrite the instruction at 2014 ...
        addi(4, 4, -1),          // 2010
        addi(3, 3, 1),           // 2014  ... which runs right after, in the same block
        cmpwi(0, 4, 0),          // 2018
        bne(-0x10),              // 201C  back to 200C
        0x00000000u,             // 2020  illegal: stops (program exception)
    };
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        store_be32(ram + base + 4 * i, prog[i]);
    m.cpu.state().pc = base;
    m.cpu.state().msr = msr::ME;
    m.cpu.run(3 + 10 * 5);
    // Every pass ran the rewritten instruction: 10 * 100.
    CHECK_EQ(m.cpu.state().gpr[3], 1000u);
    CHECK_EQ(m.cpu.state().pc, base + 0x20);
    CHECK(m.cache.stats().invalidations >= 1);
}

BOTH(dma_write_through_bus_observer)
{
    // A block is built, then the code is changed behind the CPU's back
    // (as DMA would) and reported with note_write: the next run must use
    // the new code.
    Machine m(4u << 20);
    u8* ram = m.ram.data();
    store_be32(ram + 0x3000, li(3, 1));
    store_be32(ram + 0x3004, b(-4));
    m.cpu.state().pc = 0x3000;
    m.cpu.run(10);
    CHECK_EQ(m.cpu.state().gpr[3], 1u);
    store_be32(ram + 0x3000, li(3, 2));
    m.cache.note_write(0x3000, 4);
    m.cpu.state().pc = 0x3000;
    m.cpu.run(1);
    CHECK_EQ(m.cpu.state().gpr[3], 2u);
}

BOTH(interrupt_mid_block)
{
    // The decrementer fires in the middle of a long block: the exception
    // must be taken at the same instruction as the interpreter's.
    for (u32 cut = 1; cut < 40; ++cut) {
        Machine a(0), c(4u << 20);
        for (Machine* m : {&a, &c}) {
            for (u32 i = 0; i < 64; ++i)
                store_be32(m->ram.data() + 0x4000 + 4 * i, addi(3, 3, 1));
            store_be32(m->ram.data() + 0x900, 0x00000000u);
            m->cpu.state().pc = 0x4000;
            m->cpu.state().msr = msr::ME | msr::EE;
            m->cpu.state().spr[spr::DEC] = 1;
            m->cpu.run(cut);
            m->cpu.advance_timebase(2);  // DEC 1 -> 0xFFFFFFFF: pending
            m->cpu.run(1);  // takes the interrupt
        }
        CHECK_EQ(c.cpu.state().gpr[3], a.cpu.state().gpr[3]);
        CHECK_EQ(c.cpu.state().pc, a.cpu.state().pc);
        CHECK_EQ(c.cpu.state().spr[spr::SRR0], a.cpu.state().spr[spr::SRR0]);
        CHECK_EQ(c.cpu.state().spr[spr::SRR0], 0x4000u + 4 * cut);
    }
}

namespace {
// Runs `prog` at `base` on an interpreter and on a block-cache CPU with
// the same setup, `count` instructions in slices of `slice`; checks the
// states agree. `prep` adjusts each machine first.
template <typename Prep>
void run_both(const u32* prog, u32 words, u32 base, u32 count, u32 slice, Prep prep, Machine& a, Machine& c)
{
    for (Machine* m : {&a, &c}) {
        for (u32 i = 0; i < words; ++i)
            store_be32(m->ram.data() + base + 4 * i, prog[i]);
        m->cpu.state().pc = base;
        m->cpu.state().msr = msr::ME;
        prep(*m);
        for (u32 done = 0; done < count; done += slice)
            m->cpu.run(slice);
    }
    CHECK(same(a, c, "directed", 0, 0));
}
} // namespace

BOTH(interrupt_raised_inside_block)
{
    // mftb calls the time hook, which advances the time base far enough
    // for the decrementer to fire: the interrupt becomes pending in the
    // middle of a block and must be taken right after the mftb, as the
    // interpreter does.
    const u32 prog[] = {addi(3, 3, 1), addi(3, 3, 1), mftb(5, 268), addi(3, 3, 1), addi(3, 3, 1),
                        addi(3, 3, 1), b(-24)};
    Machine a(0), c(4u << 20);
    run_both(prog, 7, 0x4000, 60, 60, [](Machine& m) {
        m.cpu.state().msr |= msr::EE;
        m.cpu.state().spr[spr::DEC] = 1;
        store_be32(m.ram.data() + 0x900, addi(7, 7, 1));  // the handler: count, then loop forever
        store_be32(m.ram.data() + 0x904, b(0));
        m.cpu.set_time_hook([](void* cpu) { static_cast<Ppc750*>(cpu)->advance_timebase(4); }, &m.cpu);
    }, a, c);
    CHECK_EQ(c.cpu.state().spr[spr::SRR0], 0x400Cu);  // after the mftb
    CHECK_EQ(c.cpu.state().gpr[7], 1u);
}

namespace {
// Places (address, word) pairs in both machines' memory.
struct Word { u32 addr, insn; };
void place(Machine& m, std::initializer_list<Word> words)
{
    for (const Word& w : words)
        store_be32(m.ram.data() + w.addr, w.insn);
}
u32 call(u32 from, u32 to) { return b(static_cast<int32_t>(to - from), false, true); }
void map_bats(Machine& m)
{
    CpuState& s = m.cpu.state();
    s.spr[spr::IBAT0U] = s.spr[spr::DBAT0U] = 0x0000001Fu;  // EA 0..1 MB = PA, read/write
    s.spr[spr::IBAT0U + 1] = s.spr[spr::DBAT0U + 1] = 0x00000002u;
    s.msr |= msr::IR | msr::DR;
    m.cpu.flush_tlb();
}
} // namespace

BOTH(page_crossing_and_reserved_stores_into_code)
{
    // With data translation on, a word store that straddles a page edge is
    // done byte by byte, and stwcx. has its own write path: both rewrite
    // code that already has blocks.
    Machine a(0), c(4u << 20);
    for (Machine* m : {&a, &c}) {
        place(*m, {
            {0x5000, li(3, 0)},           {0x5004, call(0x5004, 0x6000)},  // r3 = 1
            {0x5008, li(4, 0x3880)},      {0x500C, stw(4, 0x5FFE, 0)},     // 0x6000 becomes li r4,1
            {0x5010, li(3, 0)},           {0x5014, call(0x5014, 0x6000)},  // r4 = 1, r3 stays 0
            {0x5018, li(9, 0x7000)},      {0x501C, call(0x501C, 0x7000)},  // r5 = 5
            {0x5020, lis(10, 0x38A0)},    {0x5024, ori(10, 10, 7)},        // r10 = li r5,7
            {0x5028, lwarx(11, 0, 9)},    {0x502C, stwcx(10, 0, 9)},
            {0x5030, li(5, 0)},           {0x5034, call(0x5034, 0x7000)},  // r5 = 7
            {0x5038, 0x00000000u},
            {0x6000, li(3, 1)},           {0x6004, blr()},
            {0x7000, li(5, 5)},           {0x7004, blr()},
        });
        m->cpu.state().pc = 0x5000;
        m->cpu.state().msr = msr::ME;
        map_bats(*m);
        m->cpu.run(22);
    }
    CHECK(same(a, c, "page crossing", 0, 0));
    CHECK_EQ(c.cpu.state().gpr[3], 0u);
    CHECK_EQ(c.cpu.state().gpr[4], 1u);
    CHECK_EQ(c.cpu.state().gpr[5], 7u);
    CHECK_EQ(c.cpu.state().pc, 0x5038u);
}

BOTH(block_ends_at_page_end)
{
    // A subroutine that runs over a page edge, whose second page is then
    // rewritten: blocks stop at the page end, so the new code is seen.
    Machine a(0), c(4u << 20);
    for (Machine* m : {&a, &c}) {
        place(*m, {
            {0x5000, call(0x5000, 0x5FF8)},                            // r5 = 3
            {0x5004, lis(6, 0x38A0)},  {0x5008, ori(6, 6, 9)},         // li r5,9
            {0x500C, stw(6, 0x6000, 0)},
            {0x5010, li(5, 0)},        {0x5014, call(0x5014, 0x5FF8)}, // r5 = 9
            {0x5018, 0x00000000u},
            {0x5FF8, li(3, 1)}, {0x5FFC, li(4, 2)}, {0x6000, li(5, 3)}, {0x6004, blr()},
        });
        m->cpu.state().pc = 0x5000;
        m->cpu.state().msr = msr::ME;
        m->cpu.run(14);
    }
    CHECK(same(a, c, "page end", 0, 0));
    CHECK_EQ(c.cpu.state().gpr[5], 9u);
    CHECK_EQ(c.cpu.state().pc, 0x5018u);
}

BOTH(code_watch_same_as_interpreter)
{
    // The code watch reports page entries: the same ones, in the same
    // order, with or without the cache.
    struct Log {
        u32 pcs[64] = {};
        u32 n = 0;
    };
    Log logs[2];
    Machine a(0), c(4u << 20);
    Machine* ms[2] = {&a, &c};
    for (u32 k = 0; k < 2; ++k) {
        Machine& m = *ms[k];
        place(m, {
            {0x5000, li(7, 5)},  {0x5004, mtspr(9, 7)},                // ctr = 5
            {0x5008, call(0x5008, 0x6010)}, {0x500C, bdnz(-4)},
            {0x5010, 0x00000000u},
            {0x6010, addi(3, 3, 1)}, {0x6014, blr()},
        });
        m.cpu.state().pc = 0x5000;
        m.cpu.state().msr = msr::ME;
        m.cpu.set_code_watch(0x6000, 0x6FFF, [](void* ctx, u32 pc) {
            Log* l = static_cast<Log*>(ctx);
            if (l->n < 64)
                l->pcs[l->n++] = pc;
        }, &logs[k]);
        m.cpu.run(22);
    }
    CHECK(same(a, c, "watch", 0, 0));
    CHECK_EQ(logs[0].n, 5u);
    CHECK_EQ(logs[1].n, logs[0].n);
    CHECK(std::memcmp(logs[0].pcs, logs[1].pcs, sizeof logs[0].pcs) == 0);
    CHECK_EQ(c.cpu.state().pc, 0x5010u);
}

BOTH(break_run_and_checkstop_inside_block)
{
    // break_run() from the time hook (as the machine does after a DEC
    // write) ends run() right after that instruction; a machine check with
    // MSR[ME] = 0 stops the CPU at once. Same as the interpreter.
    const u32 prog[] = {addi(3, 3, 1), mftb(5, 268), addi(3, 3, 1), addi(3, 3, 1),
                        lwz(6, 0, 7),  // r7 = 0x80000000: outside RAM, machine check
                        addi(3, 3, 1), b(-24)};
    Machine a(0), c(4u << 20);
    for (Machine* m : {&a, &c}) {
        place(*m, {});
        for (u32 i = 0; i < 7; ++i)
            store_be32(m->ram.data() + 0x4000 + 4 * i, prog[i]);
        m->cpu.state().pc = 0x4000;
        m->cpu.state().msr = 0;  // ME = 0
        m->cpu.state().gpr[7] = 0x80000000u;
        m->cpu.set_time_hook([](void* cpu) { static_cast<Ppc750*>(cpu)->break_run(); }, &m->cpu);
        m->cpu.run(100);
        CHECK_EQ(m->cpu.instructions_executed(), 2u);  // stopped after the mftb
        m->cpu.run(100);
        CHECK(m->cpu.checkstopped());
    }
    CHECK(same(a, c, "break/checkstop", 0, 0));
    CHECK_EQ(c.cpu.state().gpr[3], 3u);
}

BOTH(external_interrupt_raised_inside_block)
{
    // A device raises the external interrupt during an instruction (here
    // from the time hook, as an MMIO write would): it must be taken right
    // after that instruction, as the interpreter does.
    const u32 prog[] = {addi(3, 3, 1), mftb(5, 268), addi(3, 3, 1), addi(3, 3, 1), addi(3, 3, 1), b(-20)};
    Machine a(0), c(4u << 20);
    for (Machine* m : {&a, &c}) {
        for (u32 i = 0; i < 6; ++i)
            store_be32(m->ram.data() + 0x4000 + 4 * i, prog[i]);
        place(*m, {{0x500, addi(7, 7, 1)}, {0x504, b(0)}});
        m->cpu.state().pc = 0x4000;
        m->cpu.state().msr = msr::ME | msr::EE;
        m->cpu.set_time_hook([](void* cpu) { static_cast<Ppc750*>(cpu)->set_external_interrupt(true); }, &m->cpu);
        m->cpu.run(40);
    }
    CHECK(same(a, c, "external", 0, 0));
    CHECK_EQ(c.cpu.state().spr[spr::SRR0], 0x4008u);  // after the mftb
    CHECK_EQ(c.cpu.state().gpr[3], 1u);
}

BOTH(code_buffer_runs_out)
{
    // A code buffer of the smallest size set_jit accepts: it fills again
    // and again; each time everything is flushed and compiled afresh.
    const u32 saved = g_jit_words;
    g_jit_words = 1024;
    Totals t;
    int failed = 0;
    for (int seed = 3001; seed <= 3040 && failed < 3; ++seed)
        failed += !differential_run(seed, seed & 1, 4u << 20, t);
    g_jit_words = saved;
    CHECK_EQ(failed, 0);
    if (g_jit)
        CHECK(t.jit_resets > 20);
}
