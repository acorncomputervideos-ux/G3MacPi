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
    auto mask = [](u64 v, bool x) -> u64 { return x ? v : (v & 0xFFFFFFFFu); };
    auto low_bits = [](u32 n) -> u64 { return n >= 64 ? ~0ull : ((1ull << n) - 1); };
    // SUBS: result and NZCV (Arm ARM AddWithCarry(a, NOT b, 1)).
    auto subs = [&](u64 a, u64 b, bool x) -> u64 {
        a = mask(a, x);
        b = mask(b, x);
        const u64 r = mask(a - b, x);
        const u32 top = x ? 63 : 31;
        n_ = (r >> top) & 1;
        z_ = r == 0;
        c_ = a >= b;  // no borrow
        v_ = (((a ^ b) & (a ^ r)) >> top) & 1;
        return r;
    };
    // ConditionHolds (Arm ARM).
    auto holds = [&](u32 cond) -> bool {
        bool r;
        switch (cond >> 1) {
        case 0: r = z_; break;
        case 1: r = c_; break;
        case 2: r = n_; break;
        case 3: r = v_; break;
        case 4: r = c_ && !z_; break;
        case 5: r = n_ == v_; break;
        case 6: r = n_ == v_ && !z_; break;
        default: r = true; break;
        }
        return ((cond & 1) && cond != 15) ? !r : r;
    };

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
            u64 imm = (i >> 10) & 0xFFF;
            if (i & (1u << 22))
                imm <<= 12;
            const bool sub = (i & (1u << 30)) != 0;
            if (i & (1u << 29)) {                              // SUBS/ADDS: Rn may be SP, Rd is ZR
                if (!sub)
                    return fail("ADDS immediate (not used)", pc, lo);
                const u64 r = subs(rs(rn), imm, x);
                wz(rd, r, x);
            } else {
                const u64 a = rs(rn);
                ws(rd, sub ? a - imm : a + imm, x);
            }
        } else if ((i & 0x1F200000u) == 0x0B000000u) {         // add/sub shifted register
            const u32 amount = (i >> 10) & 63;
            if (((i >> 22) & 3) || (!x && amount > 31))
                return fail("unsupported add/sub register form", pc, lo);
            const u64 a = rz(rn), b = mask(rz(rm) << amount, x);
            const bool sub = (i & (1u << 30)) != 0;
            if (i & (1u << 29)) {
                if (!sub)
                    return fail("ADDS register (not used)", pc, lo);
                wz(rd, subs(a, b, x), x);
            } else {
                wz(rd, sub ? a - b : a + b, x);
            }
        } else if ((i & 0x1F000000u) == 0x0A000000u) {         // logical shifted register
            const u32 opc = (i >> 29) & 3, amount = (i >> 10) & 63;
            if (opc == 3 || ((i >> 21) & 7) || (!x && amount > 31))
                return fail("unsupported logical register form", pc, lo);
            const u64 a = rz(rn), b = mask(rz(rm) << amount, x);
            wz(rd, opc == 0 ? (a & b) : opc == 1 ? (a | b) : (a ^ b), x);
        } else if ((i & 0x7F800000u) == 0x53000000u) {         // UBFM
            const bool n = (i >> 22) & 1;
            const u32 immr = (i >> 16) & 63, imms = (i >> 10) & 63, size = x ? 64 : 32;
            if (n != x || immr >= size || imms >= size)
                return fail("bad UBFM", pc, lo);
            const u64 src = mask(rz(rn), x);
            u64 r;
            if (imms >= immr)   // UBFX / LSR: bits imms..immr to the bottom
                r = (src >> immr) & low_bits(imms - immr + 1);
            else                // UBFIZ / LSL: bits imms..0 up to size - immr
                r = (src & low_bits(imms + 1)) << (size - immr);
            wz(rd, r, x);
        } else if ((i & 0x7F800000u) == 0x33000000u) {         // BFM
            const bool nbit = (i >> 22) & 1;
            const u32 immr = (i >> 16) & 63, imms = (i >> 10) & 63, size = x ? 64 : 32;
            if (nbit != x || immr >= size || imms >= size)
                return fail("bad BFM", pc, lo);
            const u64 src = mask(rz(rn), x), dst = mask(rz(rd), x);
            u64 r;
            if (imms >= immr) {  // BFXIL: bits imms..immr of src to the bottom of dst
                const u64 m = low_bits(imms - immr + 1);
                r = (dst & ~m) | ((src >> immr) & m);
            } else {             // BFI: the low imms+1 bits of src to bit size - immr
                const u32 lsb = size - immr;
                const u64 m = low_bits(imms + 1) << lsb;
                r = (dst & ~m) | ((src << lsb) & m);
            }
            wz(rd, mask(r, x), x);
        } else if ((i & 0x7FE00C00u) == 0x1A800400u) {         // CSINC
            const u64 r = holds((i >> 12) & 15) ? rz(rn) : rz(rm) + 1;
            wz(rd, r, x);
        } else if ((i & 0xFFFFF000u) == 0x5AC00000u && (((i >> 10) & 3) == 1 || ((i >> 10) & 3) == 2)) {  // REV16/REV (32)
            const u32 v = static_cast<u32>(rz(rn));
            const u32 r = ((i >> 10) & 3) == 2
                ? (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24)
                : ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
            wz(rd, r, false);
        } else if ((i & 0x3B200C00u) == 0x38200800u) {         // LDR/STR register offset
            const u32 size = i >> 30, opc = (i >> 22) & 3, option = (i >> 13) & 7;
            if ((i & (1u << 26)) || opc > 1 || size == 3 || option != 3 || (i & (1u << 12)))
                return fail("unsupported register-offset load/store", pc, lo);
            const u32 bytes = 1u << size;
            u8* addr = reinterpret_cast<u8*>(rs(rn) + rz(rm));
            if (opc == 1) {
                u64 v = 0;
                std::memcpy(&v, addr, bytes);
                wz(rd, v, false);
            } else {
                const u64 v = rz(rd);
                std::memcpy(addr, &v, bytes);
            }
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
        } else if ((i & 0xFF000010u) == 0x54000000u) {         // B.cond
            if (holds(i & 15))
                next = pc + sext((i >> 5) & 0x7FFFF, 19);
        } else if ((i & 0x7E000000u) == 0x36000000u) {         // TBZ/TBNZ
            const u32 bit = ((i >> 31) << 5) | ((i >> 19) & 31);
            const bool set = (rz(rd) >> bit) & 1;
            if (set == (((i >> 24) & 1) != 0))
                next = pc + sext((i >> 5) & 0x3FFF, 14);
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
