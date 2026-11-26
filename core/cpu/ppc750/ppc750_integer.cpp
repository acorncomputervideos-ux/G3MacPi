// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_integer.cpp — integer arithmetic, logical, shift,
// rotate, compare and trap instructions (PEM chapter 3.3 and chapter 12).

#include "core/common/bitops.h"
#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

void Ppc750::set_cr_field(u32 field_index, u32 value)
{
    const u32 shift = 28 - 4 * field_index;
    s_.cr = (s_.cr & ~(0xFu << shift)) | ((value & 0xFu) << shift);
}

void Ppc750::update_cr0(u32 result)
{
    const s32 r = static_cast<s32>(result);
    u32 f = r < 0 ? crf::LT : (r > 0 ? crf::GT : crf::EQ);
    if (s_.xer() & xer::SO)
        f |= crf::SO;
    set_cr_field(0, f);
}

void Ppc750::set_ov(bool overflow)
{
    // OV reflects this instruction; SO is sticky (PEM 2.1.5).
    if (overflow)
        s_.xer() |= xer::OV | xer::SO;
    else
        s_.xer() &= ~xer::OV;
}

// add/subtract family. xo == 0: D-form (selected by the primary opcode);
// otherwise the 9-bit XO-form extended opcode.
void Ppc750::op_add_family(u32 insn, u32 xo)
{
    const u32 rd = field::rd(insn);
    const u32 ra = field::ra(insn);
    const u32 a = s_.gpr[ra];

    if (xo == 0) {
        const u32 op = field::opcd(insn);
        const u32 imm = field::simm(insn);
        switch (op) {
        case 14:  // addi: (rA|0) + SIMM
            s_.gpr[rd] = (ra ? a : 0) + imm;
            return;
        case 15:  // addis: (rA|0) + (SIMM << 16)
            s_.gpr[rd] = (ra ? a : 0) + (imm << 16);
            return;
        case 7: {  // mulli: low 32 bits of rA * SIMM
            const s64 p = static_cast<s64>(static_cast<s32>(a)) * static_cast<s32>(imm);
            s_.gpr[rd] = static_cast<u32>(p);
            return;
        }
        case 12:    // addic
        case 13: {  // addic.
            const u64 r = static_cast<u64>(a) + imm;
            s_.gpr[rd] = static_cast<u32>(r);
            set_ca((r >> 32) != 0);
            if (op == 13)
                update_cr0(s_.gpr[rd]);
            return;
        }
        case 8: {  // subfic: ~rA + SIMM + 1
            const u64 r = static_cast<u64>(~a) + imm + 1;
            s_.gpr[rd] = static_cast<u32>(r);
            set_ca((r >> 32) != 0);
            return;
        }
        default:
            illegal(insn);
            return;
        }
    }

    // XO-form: result = x + y + carry_in.
    const u32 b = s_.gpr[field::rb(insn)];
    u32 x = a, y = b, cin = 0;
    bool writes_ca = true;
    switch (xo) {
    case 266: writes_ca = false; break;                     // add
    case 10:  break;                                        // addc
    case 138: cin = ca(); break;                            // adde
    case 234: y = 0xFFFFFFFFu; cin = ca(); break;           // addme
    case 202: y = 0; cin = ca(); break;                     // addze
    case 40:  x = ~a; cin = 1; writes_ca = false; break;    // subf
    case 8:   x = ~a; cin = 1; break;                       // subfc
    case 136: x = ~a; cin = ca(); break;                    // subfe
    case 232: x = ~a; y = 0xFFFFFFFFu; cin = ca(); break;   // subfme
    case 200: x = ~a; y = 0; cin = ca(); break;             // subfze
    case 104: x = ~a; y = 0; cin = 1; writes_ca = false; break;  // neg
    default:
        illegal(insn);
        return;
    }

    const u64 wide = static_cast<u64>(x) + y + cin;
    const u32 r = static_cast<u32>(wide);
    s_.gpr[rd] = r;
    if (writes_ca)
        set_ca((wide >> 32) != 0);
    if (field::oe(insn))
        set_ov((((x ^ r) & (y ^ r)) >> 31) != 0);
    if (field::rc(insn))
        update_cr0(r);
}

void Ppc750::op_mul_div(u32 insn, u32 xo)
{
    const u32 rd = field::rd(insn);
    const u32 a = s_.gpr[field::ra(insn)];
    const u32 b = s_.gpr[field::rb(insn)];
    const s32 sa = static_cast<s32>(a);
    const s32 sb = static_cast<s32>(b);
    u32 r = 0;
    bool overflow = false;

    switch (xo) {
    case 235: {  // mullw
        const s64 p = static_cast<s64>(sa) * sb;
        r = static_cast<u32>(p);
        overflow = p != static_cast<s64>(static_cast<s32>(r));
        break;
    }
    case 75: {  // mulhw
        const s64 p = static_cast<s64>(sa) * sb;
        r = static_cast<u32>(static_cast<u64>(p) >> 32);
        break;
    }
    case 11: {  // mulhwu
        const u64 p = static_cast<u64>(a) * b;
        r = static_cast<u32>(p >> 32);
        break;
    }
    case 491:  // divw
        if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) {
            // Architecturally undefined result. Value observed in the
            // hardware-derived vectors (tests/vectors/dingusppc): -1 if the
            // dividend is negative, otherwise 0.
            r = (a & 0x80000000u) ? 0xFFFFFFFFu : 0u;
            overflow = true;
        } else {
            r = static_cast<u32>(sa / sb);
        }
        break;
    case 459:  // divwu
        if (b == 0) {
            r = 0;  // undefined architecturally; 0 observed in the vectors
            overflow = true;
        } else {
            r = a / b;
        }
        break;
    default:
        illegal(insn);
        return;
    }

    s_.gpr[rd] = r;
    if (field::oe(insn) && xo != 75 && xo != 11)
        set_ov(overflow);
    if (field::rc(insn))
        update_cr0(r);
}

void Ppc750::op_logical(u32 insn, u32 xo)
{
    const u32 rs = s_.gpr[field::rd(insn)];
    const u32 ra = field::ra(insn);

    if (xo == 0) {  // D-form, destination rA
        const u32 imm = field::uimm(insn);
        switch (field::opcd(insn)) {
        case 24: s_.gpr[ra] = rs | imm; break;                                    // ori
        case 25: s_.gpr[ra] = rs | (imm << 16); break;                            // oris
        case 26: s_.gpr[ra] = rs ^ imm; break;                                    // xori
        case 27: s_.gpr[ra] = rs ^ (imm << 16); break;                            // xoris
        case 28: s_.gpr[ra] = rs & imm; update_cr0(s_.gpr[ra]); break;            // andi.
        case 29: s_.gpr[ra] = rs & (imm << 16); update_cr0(s_.gpr[ra]); break;    // andis.
        default: illegal(insn); break;
        }
        return;
    }

    const u32 rb = s_.gpr[field::rb(insn)];
    u32 r;
    switch (xo) {
    case 28:  r = rs & rb; break;                       // and
    case 60:  r = rs & ~rb; break;                      // andc
    case 124: r = ~(rs | rb); break;                    // nor
    case 284: r = ~(rs ^ rb); break;                    // eqv
    case 316: r = rs ^ rb; break;                       // xor
    case 412: r = rs | ~rb; break;                      // orc
    case 444: r = rs | rb; break;                       // or
    case 476: r = ~(rs & rb); break;                    // nand
    case 26:  r = count_leading_zeros32(rs); break;     // cntlzw
    case 922: r = static_cast<u32>(sign_extend16(rs)); break;  // extsh
    case 954: r = static_cast<u32>(sign_extend8(rs)); break;   // extsb
    default:
        illegal(insn);
        return;
    }
    s_.gpr[ra] = r;
    if (field::rc(insn))
        update_cr0(r);
}

void Ppc750::op_shift(u32 insn, u32 xo)
{
    const u32 rs = s_.gpr[field::rd(insn)];
    const u32 ra = field::ra(insn);
    u32 r;

    switch (xo) {
    case 24: {  // slw: shift count is rB[26-31] (6 bits); >= 32 gives 0
        const u32 n = s_.gpr[field::rb(insn)] & 0x3F;
        r = n > 31 ? 0 : rs << n;
        break;
    }
    case 536: {  // srw
        const u32 n = s_.gpr[field::rb(insn)] & 0x3F;
        r = n > 31 ? 0 : rs >> n;
        break;
    }
    case 792:    // sraw
    case 824: {  // srawi
        const u32 n = (xo == 824) ? field::rb(insn) : (s_.gpr[field::rb(insn)] & 0x3F);
        const bool negative = (rs & 0x80000000u) != 0;
        if (n > 31) {
            r = negative ? 0xFFFFFFFFu : 0u;
            set_ca(negative);
        } else {
            r = static_cast<u32>(static_cast<s32>(rs) >> n);
            // CA = 1 if negative and any 1-bits were shifted out.
            const u32 lost = n == 0 ? 0 : (rs & ((1u << n) - 1u));
            set_ca(negative && lost != 0);
        }
        break;
    }
    default:
        illegal(insn);
        return;
    }
    s_.gpr[ra] = r;
    if (field::rc(insn))
        update_cr0(r);
}

void Ppc750::op_rotate(u32 insn, u32 opcode)
{
    const u32 rs = s_.gpr[field::rd(insn)];
    const u32 ra = field::ra(insn);
    const u32 m = ppc_mask(field::mb(insn), field::me(insn));
    u32 r;

    switch (opcode) {
    case 21:  // rlwinm
        r = rotl32(rs, field::rb(insn)) & m;
        break;
    case 23:  // rlwnm
        r = rotl32(rs, s_.gpr[field::rb(insn)] & 31) & m;
        break;
    case 20:  // rlwimi
        r = (rotl32(rs, field::rb(insn)) & m) | (s_.gpr[ra] & ~m);
        break;
    default:
        illegal(insn);
        return;
    }
    s_.gpr[ra] = r;
    if (field::rc(insn))
        update_cr0(r);
}

void Ppc750::op_compare(u32 crf_index, u32 a, u32 b, bool is_signed)
{
    u32 f;
    if (is_signed) {
        const s32 sa = static_cast<s32>(a), sb = static_cast<s32>(b);
        f = sa < sb ? crf::LT : (sa > sb ? crf::GT : crf::EQ);
    } else {
        f = a < b ? crf::LT : (a > b ? crf::GT : crf::EQ);
    }
    if (s_.xer() & xer::SO)
        f |= crf::SO;
    set_cr_field(crf_index, f);
}

void Ppc750::op_trap(u32 to, u32 a, u32 b)
{
    const s32 sa = static_cast<s32>(a), sb = static_cast<s32>(b);
    const bool trap = ((to & 0x10) && sa < sb) || ((to & 0x08) && sa > sb) ||
                      ((to & 0x04) && a == b) || ((to & 0x02) && a < b) ||
                      ((to & 0x01) && a > b);
    if (trap)
        raise_exception(Exception::Program, s_.pc, srr1::TRAP);
}

} // namespace g3::ppc
