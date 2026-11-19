// SPDX-License-Identifier: GPL-3.0-or-later
//
// The A64 simulator (tools/a64sim) the JIT tests run generated code on.
// Each instruction against results worked out by hand from the Arm ARM
// definitions, and its strictness: unknown encodings, misaligned calls and
// clobbered caller-saved registers are caught.

#include "core/cpu/ppc750/jit/a64_assembler.h"
#include "tests/framework/g3test.h"
#include "tools/a64sim/a64_sim.h"

#include <vector>

using namespace g3;
using namespace g3::a64;

namespace {
struct Prog {
    std::vector<u32> buf = std::vector<u32>(4096);
    Assembler a{buf.data(), 4096};
    Simulator sim;
    bool run(u64 arg)
    {
        if (!a.ok())
            return false;
        return sim.call(buf.data(), arg, buf.data(), buf.data() + buf.size());
    }
};
u64 g_results[16];
void host_fn(void* p, u32 w)
{
    g_results[0] = reinterpret_cast<u64>(p);
    g_results[1] = w;
}
} // namespace

TEST_CASE("cpu/a64sim/moves_and_arithmetic")
{
    Prog p;
    Assembler& a = p.a;
    // x0 = pointer to 16 result slots.
    const u32 vals[] = {0, 1, 0xFFFF, 0x10000, 0x12345678, 0xFFFFFFFF, 0xFFFF1234, 0x1234FFFF, 0x80000000, 0x7FFF8000};
    for (u32 k = 0; k < 10; ++k) {
        a.mov_imm32(X8, vals[k]);
        a.str(true, X8, X0, 8 * k);  // 64-bit store: upper half must be zero
    }
    a.mov_imm64(X9, 0x0123456789ABCDEFull);
    a.str(true, X9, X0, 80);
    a.mov_imm32(X10, 0xFFFFFFF0);
    a.add_imm(false, X10, X10, 0x20);         // wraps to 0x10
    a.sub_imm(false, X11, X10, 0x11);         // 0xFFFFFFFF
    a.str(true, X10, X0, 88);
    a.str(true, X11, X0, 96);
    a.mov_imm32(X12, 0x80000001);
    a.ror_imm(X12, X12, 1);                   // 0xC0000000
    a.str(false, X12, X0, 104);
    a.mov_imm32(X13, 0xF0F0);
    a.mov_imm32(X14, 0xFF00);
    a.and_(false, X15, X13, X14);             // 0xF000
    a.orr(false, X13, X13, X14);              // 0xFFF0
    a.eor(false, X14, X13, X14);              // 0x00F0
    a.str(false, X15, X0, 108);
    a.str(false, X13, X0, 112);
    a.str(false, X14, X0, 116);
    a.sub(false, X15, X14, X13);              // 0xF0 - 0xFFF0 = 0xFFFF0100
    a.str(false, X15, X0, 120);
    a.ret();
    u64 out[16] = {};
    CHECK(p.run(reinterpret_cast<u64>(out)));
    for (u32 k = 0; k < 10; ++k)
        CHECK_EQ(out[k], static_cast<u64>(vals[k]));
    CHECK_EQ(out[10], 0x0123456789ABCDEFull);
    CHECK_EQ(out[11], 0x10u);
    CHECK_EQ(out[12], 0xFFFFFFFFu);
    const u32* w = reinterpret_cast<const u32*>(&out[13]);
    CHECK_EQ(w[0], 0xC0000000u);
    CHECK_EQ(w[1], 0xF000u);
    CHECK_EQ(w[2], 0xFFF0u);
    CHECK_EQ(w[3], 0x00F0u);
    CHECK_EQ(w[4], 0xFFFF0100u);
}

TEST_CASE("cpu/a64sim/stack_branches_and_host_calls")
{
    Prog p;
    Assembler& a = p.a;
    Assembler::Label skip, loop;
    a.stp_pre(X29, X30, SP, -32);
    a.stp(X19, X20, SP, 16);
    a.mov(true, X19, X0);
    a.movz(false, X20, 3, 0);
    a.bind(loop);                       // three host calls with w1 = 3, 2, 1
    a.mov(true, X0, X19);
    a.mov(false, X1, X20);
    a.mov_imm64(X16, reinterpret_cast<u64>(&host_fn));
    a.blr(X16);
    a.sub_imm(false, X20, X20, 1);
    a.cbnz(false, X20, skip);           // forward when not zero ...
    Assembler::Label done;
    a.b(done);
    a.bind(skip);
    a.b(loop);                          // ... then back
    a.bind(done);
    a.ldrb(X8, X19, 0);
    a.strb(X8, X19, 1);
    a.ldp(X19, X20, SP, 16);
    a.ldp_post(X29, X30, SP, 32);
    a.ret();
    u8 data[8] = {0x5A};
    CHECK(p.run(reinterpret_cast<u64>(data)));
    CHECK_EQ(p.sim.host_calls(), 3u);
    CHECK_EQ(g_results[0], reinterpret_cast<u64>(data));
    CHECK_EQ(g_results[1], 1u);
    CHECK_EQ(data[1], 0x5Au);
}

TEST_CASE("cpu/a64sim/strict")
{
    {   // misaligned stack at a call
        Prog p;
        p.a.sub_imm(true, SP, SP, 8);
        p.a.mov_imm64(X16, reinterpret_cast<u64>(&host_fn));
        p.a.blr(X16);
        p.a.ret();
        CHECK(!p.run(0));
    }
    {   // an encoding outside the subset (ADDS)
        Prog p;
        p.a.emit(0x2B020020u);  // adds w0, w1, w2
        p.a.ret();
        CHECK(!p.run(0));
    }
    {   // a callee-saved register not restored
        Prog p;
        p.a.movz(false, X21, 1, 0);
        p.a.ret();
        CHECK(!p.run(0));
    }
    {   // returning with SP moved
        Prog p;
        p.a.sub_imm(true, SP, SP, 16);
        p.a.ret();
        CHECK(!p.run(0));
    }
    {   // x9 is poisoned after a call: storing it shows the poison value
        Prog p;
        p.a.stp_pre(X19, X30, SP, -16);
        p.a.mov(true, X19, X0);
        p.a.movz(false, X9, 7, 0);
        p.a.mov_imm64(X16, reinterpret_cast<u64>(&host_fn));
        p.a.blr(X16);
        p.a.str(true, X9, X19, 0);
        p.a.ldp_post(X19, X30, SP, 16);
        p.a.ret();
        u64 v = 0;
        CHECK(p.run(reinterpret_cast<u64>(&v)));
        CHECK(v != 7u);
    }
}

TEST_CASE("cpu/a64sim/compares_and_conditions")
{
    // For each pair and condition: a program that returns 1 in out[0] when
    // the branch is taken; compared with the C++ comparison.
    const u32 vals[] = {0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 1234};
    struct C { Assembler::Cond c; bool (*f)(u32, u32); };
    const C conds[] = {
        {Assembler::EQ, [](u32 a, u32 b) { return a == b; }},
        {Assembler::NE, [](u32 a, u32 b) { return a != b; }},
        {Assembler::HS, [](u32 a, u32 b) { return a >= b; }},
        {Assembler::LO, [](u32 a, u32 b) { return a < b; }},
        {Assembler::HI, [](u32 a, u32 b) { return a > b; }},
        {Assembler::LS, [](u32 a, u32 b) { return a <= b; }},
        {Assembler::GE, [](u32 a, u32 b) { return static_cast<s32>(a) >= static_cast<s32>(b); }},
        {Assembler::LT, [](u32 a, u32 b) { return static_cast<s32>(a) < static_cast<s32>(b); }},
        {Assembler::GT, [](u32 a, u32 b) { return static_cast<s32>(a) > static_cast<s32>(b); }},
        {Assembler::LE, [](u32 a, u32 b) { return static_cast<s32>(a) <= static_cast<s32>(b); }},
        {Assembler::MI, [](u32 a, u32 b) { return static_cast<s32>(a - b) < 0; }},
        {Assembler::PL, [](u32 a, u32 b) { return static_cast<s32>(a - b) >= 0; }},
        {Assembler::VS, [](u32 a, u32 b) { const s64 d = static_cast<s64>(static_cast<s32>(a)) - static_cast<s32>(b); return d != static_cast<s32>(d); }},
        {Assembler::VC, [](u32 a, u32 b) { const s64 d = static_cast<s64>(static_cast<s32>(a)) - static_cast<s32>(b); return d == static_cast<s32>(d); }},
    };
    u32 bad = 0;
    for (u32 a : vals)
        for (u32 b : vals)
            for (const C& c : conds) {
                Prog p;
                Assembler::Label taken, done;
                p.a.mov_imm32(X8, a);
                p.a.mov_imm32(X9, b);
                p.a.cmp(false, X8, X9);
                p.a.b_cond(c.c, taken);
                p.a.movz(false, X10, 0, 0);
                p.a.b(done);
                p.a.bind(taken);
                p.a.movz(false, X10, 1, 0);
                p.a.bind(done);
                p.a.str(false, X10, X0, 0);
                p.a.ret();
                u32 out = 7;
                REQUIRE(p.run(reinterpret_cast<u64>(&out)));
                bad += out != (c.f(a, b) ? 1u : 0u);
            }
    CHECK_EQ(bad, 0u);
    // cmp with an immediate: HI for unsigned greater.
    Prog p;
    Assembler::Label hi;
    p.a.mov_imm32(X8, 0xFFD);
    p.a.cmp_imm(false, X8, 0xFFC);
    p.a.b_cond(Assembler::HI, hi);
    p.a.ret();
    p.a.bind(hi);
    p.a.str(false, X8, X0, 0);
    p.a.ret();
    u32 out = 0;
    CHECK(p.run(reinterpret_cast<u64>(&out)));
    CHECK_EQ(out, 0xFFDu);
}

TEST_CASE("cpu/a64sim/bitfields_bytes_and_register_offsets")
{
    Prog p;
    Assembler& a = p.a;
    a.mov_imm32(X8, 0x12345678);
    a.lsr_imm(X9, X8, 12);          // 0x12345
    a.ubfx(X10, X8, 4, 8);          // 0x67
    a.ubfx(X11, X8, 0, 12);         // 0x678
    a.rev(X12, X8);                 // 0x78563412
    a.rev16(X13, X8);               // 0x34127856
    a.add(true, X14, X9, X9, 1);    // 3 * 0x12345
    a.orr(false, X15, X10, X11, 8); // 0x67 | 0x67800
    a.str(false, X9, X0, 0);
    a.str(false, X10, X0, 4);
    a.str(false, X11, X0, 8);
    a.str(false, X12, X0, 12);
    a.str(false, X13, X0, 16);
    a.str(false, X14, X0, 20);
    a.str(false, X15, X0, 24);
    // Register offsets: x1 = buffer, index 3.
    a.add_imm(true, X1, X0, 64);
    a.movz(true, X2, 3, 0);
    a.str_reg(4, X8, X1, X2);       // bytes 3..6 = 78 56 34 12 (little-endian)
    a.ldr_reg(1, X3, X1, X2);       // 0x78
    a.ldr_reg(2, X4, X1, X2);       // 0x5678
    a.ldr_reg(4, X5, X1, X2);       // 0x12345678
    a.str(false, X3, X0, 28);
    a.str(false, X4, X0, 32);
    a.str(false, X5, X0, 36);
    a.mov_imm32(X6, 0xAABBCCDD);
    a.str_reg(2, X6, X1, X2);       // halfword: DD CC
    a.str_reg(1, X6, X1, X2);       // byte: DD
    // tbz/tbnz
    Assembler::Label t1, t2;
    a.movz(false, X7, 0, 0);
    a.tbnz(X8, 4, t1);              // bit 4 of 0x...78 = 1: taken
    a.movz(false, X7, 9, 0);
    a.bind(t1);
    a.tbz(X8, 3, t2);               // bit 3 = 1: not taken
    a.add_imm(false, X7, X7, 2);
    a.bind(t2);
    a.str(false, X7, X0, 40);
    a.ret();
    u32 out[32] = {};
    CHECK(p.run(reinterpret_cast<u64>(out)));
    CHECK_EQ(out[0], 0x12345u);
    CHECK_EQ(out[1], 0x67u);
    CHECK_EQ(out[2], 0x678u);
    CHECK_EQ(out[3], 0x78563412u);
    CHECK_EQ(out[4], 0x34127856u);
    CHECK_EQ(out[5], 3u * 0x12345u);
    CHECK_EQ(out[6], 0x67867u);
    CHECK_EQ(out[7], 0x78u);
    CHECK_EQ(out[8], 0x5678u);
    CHECK_EQ(out[9], 0x12345678u);
    const u8* buf = reinterpret_cast<const u8*>(out) + 64;
    CHECK_EQ(buf[3], 0xDDu);
    CHECK_EQ(buf[4], 0xCCu);
    CHECK_EQ(buf[5], 0x34u);
    CHECK_EQ(out[10], 2u);
}

TEST_CASE("cpu/a64sim/cset_and_bfi")
{
    Prog p;
    Assembler& a = p.a;
    a.mov_imm32(X8, 5);
    a.mov_imm32(X9, 0xFFFFFFFF);
    a.cmp(false, X8, X9);           // 5 vs -1: signed GT, unsigned LO
    a.cset(X10, Assembler::LT);     // 0
    a.cset(X11, Assembler::GT);     // 1
    a.cset(X12, Assembler::LO);     // 1
    a.cset(X13, Assembler::EQ);     // 0
    a.mov_imm32(X14, 0x12345678);
    a.movz(false, X15, 0xA, 0);
    a.bfi(X14, X15, 28, 4);         // 0xA2345678
    a.mov_imm32(X1, 0x12345678);
    a.bfi(X1, X15, 0, 4);           // 0x1234567A
    a.mov_imm32(X2, 0xFFFFFFFF);
    a.movz(false, X3, 0x5, 0);
    a.bfi(X2, X3, 12, 4);           // 0xFFFF5FFF
    a.str(false, X10, X0, 0);
    a.str(false, X11, X0, 4);
    a.str(false, X12, X0, 8);
    a.str(false, X13, X0, 12);
    a.str(false, X14, X0, 16);
    a.str(false, X1, X0, 20);
    a.str(false, X2, X0, 24);
    a.ret();
    u32 out[8] = {9, 9, 9, 9};
    CHECK(p.run(reinterpret_cast<u64>(out)));
    CHECK_EQ(out[0], 0u);
    CHECK_EQ(out[1], 1u);
    CHECK_EQ(out[2], 1u);
    CHECK_EQ(out[3], 0u);
    CHECK_EQ(out[4], 0xA2345678u);
    CHECK_EQ(out[5], 0x1234567Au);
    CHECK_EQ(out[6], 0xFFFF5FFFu);
}
