// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_branch.cpp — branch and condition-register logical
// instructions (PEM 4.2.4, chapter 12: b, bc, bclr, bcctr, cr*).

#include "core/common/bitops.h"
#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

namespace {
// BO field bits (PEM Table 4-20), PowerPC numbering BO[0..4] = 0x10..0x01.
constexpr u32 kBoIgnoreCond = 0x10;  // BO[0]: don't test the CR bit
constexpr u32 kBoCondTrue   = 0x08;  // BO[1]: branch if CR bit == 1
constexpr u32 kBoNoDecr     = 0x04;  // BO[2]: don't decrement CTR
constexpr u32 kBoCtrZero    = 0x02;  // BO[3]: branch if CTR == 0 (else != 0)
} // namespace

void Ppc750::set_cr_bit(u32 bit, bool value)
{
    const u32 mask = 0x80000000u >> bit;
    s_.cr = value ? (s_.cr | mask) : (s_.cr & ~mask);
}

void Ppc750::op_b(u32 insn)
{
    const u32 li = static_cast<u32>(sign_extend(insn & 0x03FFFFFCu, 26));
    const u32 target = field::aa(insn) ? li : s_.pc + li;
    if (field::lk(insn))
        s_.lr() = s_.pc + 4;
    next_pc_ = target;
}

void Ppc750::op_bc(u32 insn)
{
    const u32 bo = field::rd(insn);
    const u32 bi = field::ra(insn);

    if (!(bo & kBoNoDecr))
        s_.ctr() -= 1;
    const bool ctr_ok = (bo & kBoNoDecr) || ((s_.ctr() != 0) != ((bo & kBoCtrZero) != 0));
    const bool cond_ok = (bo & kBoIgnoreCond) || (cr_bit(bi) == ((bo & kBoCondTrue) != 0));

    if (field::lk(insn))
        s_.lr() = s_.pc + 4;
    if (ctr_ok && cond_ok) {
        const u32 bd = static_cast<u32>(sign_extend(insn & 0xFFFCu, 16));
        next_pc_ = field::aa(insn) ? bd : s_.pc + bd;
    }
}

void Ppc750::op_bclr_bcctr(u32 insn, bool to_ctr)
{
    const u32 bo = field::rd(insn);
    const u32 bi = field::ra(insn);

    // bcctr with BO[2] = 0 is an invalid form (PEM bcctr); the CTR is not
    // decremented here in that case.
    bool ctr_ok = true;
    if (!to_ctr) {
        if (!(bo & kBoNoDecr))
            s_.ctr() -= 1;
        ctr_ok = (bo & kBoNoDecr) || ((s_.ctr() != 0) != ((bo & kBoCtrZero) != 0));
    }
    const bool cond_ok = (bo & kBoIgnoreCond) || (cr_bit(bi) == ((bo & kBoCondTrue) != 0));

    // Read the target before LK updates LR (bclrl uses the old LR).
    const u32 target = (to_ctr ? s_.ctr() : s_.lr()) & ~3u;
    if (field::lk(insn))
        s_.lr() = s_.pc + 4;
    if (ctr_ok && cond_ok)
        next_pc_ = target;
}

void Ppc750::op_cr_logical(u32 insn, u32 xo)
{
    const bool a = cr_bit(field::ra(insn));
    const bool b = cr_bit(field::rb(insn));
    bool r;
    switch (xo) {
    case 257: r = a && b; break;        // crand
    case 129: r = a && !b; break;       // crandc
    case 289: r = a == b; break;        // creqv
    case 225: r = !(a && b); break;     // crnand
    case 33:  r = !(a || b); break;     // crnor
    case 449: r = a || b; break;        // cror
    case 417: r = a || !b; break;       // crorc
    case 193: r = a != b; break;        // crxor
    default:
        illegal(insn);
        return;
    }
    set_cr_bit(field::rd(insn), r);
}

} // namespace g3::ppc
