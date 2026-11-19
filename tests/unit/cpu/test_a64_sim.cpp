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
