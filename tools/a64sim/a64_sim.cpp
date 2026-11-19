// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/a64sim/a64_sim.cpp — see a64_sim.h.

#include "tools/a64sim/a64_sim.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3::a64 {

namespace {
constexpr u64 kReturnSentinel = 0;         // x30 at entry: RET to it ends the call
constexpr u64 kPoison = 0xDEAD0000BAD0F00Dull;
constexpr u64 kMaxSteps = 1000000;          // per call: generated blocks are short
s64 sext(u64 v, u32 bits) { return static_cast<s64>(v << (64 - bits)) >> (64 - bits); }
} // namespace

bool Simulator::fail(const char* why, const u32* pc, const void* lo)
{
    error_ = why;
    error_off_ = static_cast<u32>(pc - static_cast<const u32*>(lo));
    return false;
}

bool Simulator::call(const u32* entry, u64 arg, const void* code_lo, const void* code_hi)
{
    error_ = nullptr;
    for (u32 i = 0; i < 31; ++i)
        x_[i] = kPoison + i;
    x_[0] = arg;
    x_[30] = kReturnSentinel;
    x_[31] = reinterpret_cast<u64>(stack_ + sizeof stack_);
    const u32* lo = static_cast<const u32*>(code_lo);
    const u32* hi = static_cast<const u32*>(code_hi);
    const u32* pc = entry;
    // Register access: r31 is ZR or SP depending on the instruction.
    auto rz = [&](u32 r) -> u64 { return r == 31 ? 0 : x_[r]; };
    auto wz = [&](u32 r, u64 v, bool x) { if (r != 31) x_[r] = x ? v : (v & 0xFFFFFFFFu); };
    auto rs = [&](u32 r) -> u64 { return x_[r]; };  // r31 = SP
    auto ws = [&](u32 r, u64 v, bool x) { x_[r] = x ? v : (v & 0xFFFFFFFFu); };

    for (u64 steps = 0; steps < kMaxSteps; ++steps) {
        if (pc < lo || pc >= hi)
            return fail("fetch outside the code buffer", pc, lo);
        const u32 i = *pc;
        const u32 rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
        const bool x = (i >> 31) != 0;
        const u32* next = pc + 1;
        ++executed_;

        if ((i & 0x1F800000u) == 0x12800000u) {                // move wide
            const u32 opc = (i >> 29) & 3, hw = (i >> 21) & 3;
            if (opc == 1 || (!x && hw > 1))
                return fail("bad move-wide", pc, lo);
            const u64 imm = static_cast<u64>((i >> 5) & 0xFFFF) << (16 * hw);
            if (opc == 0)
                wz(rd, ~imm, x);
            else if (opc == 2)
                wz(rd, imm, x);
            else
                wz(rd, (rz(rd) & ~(0xFFFFull << (16 * hw))) | imm, x);
        } else if ((i & 0x1F000000u) == 0x11000000u) {         // add/sub immediate
            if (i & (1u << 29))
                return fail("flag-setting add/sub immediate", pc, lo);
            u64 imm = (i >> 10) & 0xFFF;
            if (i & (1u << 22))
                imm <<= 12;
            const u64 a = rs(rn);
            ws(rd, (i & (1u << 30)) ? a - imm : a + imm, x);
        } else if ((i & 0x1F200000u) == 0x0B000000u) {         // add/sub shifted register
            if ((i & (1u << 29)) || ((i >> 22) & 3) || ((i >> 10) & 63))
                return fail("unsupported add/sub register form", pc, lo);
            const u64 a = rz(rn), b = rz(rm);
            wz(rd, (i & (1u << 30)) ? a - b : a + b, x);
        } else if ((i & 0x1F000000u) == 0x0A000000u) {         // logical shifted register
            const u32 opc = (i >> 29) & 3;
            if (opc == 3 || ((i >> 21) & 7) || ((i >> 10) & 63))
                return fail("unsupported logical register form", pc, lo);
            const u64 a = rz(rn), b = rz(rm);
            wz(rd, opc == 0 ? (a & b) : opc == 1 ? (a | b) : (a ^ b), x);
        } else if ((i & 0x7FA00000u) == 0x13800000u) {         // EXTR
            const bool n = (i >> 22) & 1;
            const u32 lsb = (i >> 10) & 63;
            if (n != x || (!x && lsb > 31))
                return fail("bad EXTR", pc, lo);
            if (x) {
                const u64 a = rz(rn), b = rz(rm);
                wz(rd, lsb ? (b >> lsb) | (a << (64 - lsb)) : b, true);
            } else {
                const u64 cat = (rz(rn) & 0xFFFFFFFFu) << 32 | (rz(rm) & 0xFFFFFFFFu);
                wz(rd, cat >> lsb, false);
            }
        } else if ((i & 0x3B000000u) == 0x39000000u) {         // LDR/STR unsigned offset
            const u32 size = i >> 30, opc = (i >> 22) & 3;
            if ((i & (1u << 26)) || opc > 1 || size == 1)
                return fail("unsupported load/store form", pc, lo);
            const u32 bytes = 1u << size;
            u8* addr = reinterpret_cast<u8*>(rs(rn) + (((i >> 10) & 0xFFF) * bytes));
            if (opc == 1) {
                u64 v = 0;
                std::memcpy(&v, addr, bytes);
                wz(rd, v, true);
            } else {
                const u64 v = rz(rd);
                std::memcpy(addr, &v, bytes);
            }
        } else if ((i & 0xFC000000u) == 0xA8000000u) {         // LDP/STP 64-bit
            const u32 mode = (i >> 23) & 7;
            const bool load = (i >> 22) & 1;
            if (mode < 1 || mode > 3)
                return fail("unsupported pair form", pc, lo);
            const s64 off = sext((i >> 15) & 0x7F, 7) * 8;
            const u32 rt2 = (i >> 10) & 31;
            const u64 base = rs(rn);
            const u64 addr = mode == 1 ? base : base + off;
            u64* p = reinterpret_cast<u64*>(addr);
            if (load) {
                wz(rd, p[0], true);
                wz(rt2, p[1], true);
            } else {
                p[0] = rz(rd);
                p[1] = rz(rt2);
            }
            if (mode != 2)
                ws(rn, base + off, true);
        } else if ((i & 0xFFFFFC1Fu) == 0xD63F0000u) {         // BLR
            const u64 target = rz(rn);
            if (x_[31] & 15)
                return fail("call with a misaligned stack", pc, lo);
            const void* t = reinterpret_cast<const void*>(target);
            if (t >= code_lo && t < code_hi)
                return fail("BLR into the code buffer (not used by the JIT)", pc, lo);
            auto fn = reinterpret_cast<void (*)(void*, u32)>(target);
            ++host_calls_;
            fn(reinterpret_cast<void*>(x_[0]), static_cast<u32>(x_[1]));
            for (u32 r = 0; r <= 18; ++r)
                x_[r] = kPoison + r;
            x_[30] = reinterpret_cast<u64>(next);
        } else if (i == 0xD65F03C0u) {                         // RET
            if (x_[30] == kReturnSentinel) {
                if (x_[31] != reinterpret_cast<u64>(stack_ + sizeof stack_))
                    return fail("returned with the stack pointer moved", pc, lo);
                for (u32 r = 19; r <= 29; ++r)  // callee-saved (AAPCS64)
                    if (x_[r] != kPoison + r)
                        return fail("returned with a callee-saved register changed", pc, lo);
                return true;
            }
            next = reinterpret_cast<const u32*>(x_[30]);
        } else if (i == 0xD503201Fu) {                         // NOP
        } else if ((i & 0x7E000000u) == 0x34000000u) {         // CBZ/CBNZ
            const u64 v = x ? rz(rd) : (rz(rd) & 0xFFFFFFFFu);
            const bool nz = (i >> 24) & 1;
            if ((v != 0) == nz)
                next = pc + sext((i >> 5) & 0x7FFFF, 19);
        } else if ((i & 0xFC000000u) == 0x14000000u) {         // B
            next = pc + sext(i & 0x3FFFFFF, 26);
        } else {
            return fail("instruction outside the simulated subset", pc, lo);
        }
        pc = next;
    }
    return fail("too many steps", pc, lo);
}

void simulate_jit_block(void* code, void* cpu, void* context)
{
    auto* range = static_cast<const void* const*>(context);  // {code_lo, code_hi}
    static Simulator sim;
    if (!sim.call(static_cast<const u32*>(code), reinterpret_cast<u64>(cpu), range[0], range[1])) {
        std::fprintf(stderr, "a64 simulator: %s at code word %u (0x%08X)\n", sim.error(), sim.error_pc_offset(),
                     static_cast<const u32*>(range[0])[sim.error_pc_offset()]);
        std::abort();
    }
}

} // namespace g3::a64
