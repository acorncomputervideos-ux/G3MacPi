// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/cpu/ppc_asm.h — tiny PowerPC instruction encoder for tests.
//
// Written from the instruction formats in the PEM (Appendix A), independently
// of the interpreter's decoder, so encoder and decoder check each other.

#ifndef G3_TESTS_UNIT_CPU_PPC_ASM_H
#define G3_TESTS_UNIT_CPU_PPC_ASM_H

#include "core/common/types.h"

namespace asm_ppc {

using g3::u32;

// --- formats -----------------------------------------------------------------
constexpr u32 D(u32 op, u32 rt, u32 ra, u32 imm16)
{
    return (op << 26) | (rt << 21) | (ra << 16) | (imm16 & 0xFFFF);
}
constexpr u32 X(u32 op, u32 rt, u32 ra, u32 rb, u32 xo, u32 rc = 0)
{
    return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
constexpr u32 M(u32 op, u32 rs, u32 ra, u32 sh, u32 mb, u32 me, u32 rc = 0)
{
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc;
}
constexpr u32 spr_field(u32 spr) { return ((spr & 0x1F) << 5) | ((spr >> 5) & 0x1F); }

// --- integer -----------------------------------------------------------------
constexpr u32 addi(u32 rd, u32 ra, u32 simm) { return D(14, rd, ra, simm); }
constexpr u32 li(u32 rd, u32 simm) { return addi(rd, 0, simm); }
constexpr u32 addis(u32 rd, u32 ra, u32 simm) { return D(15, rd, ra, simm); }
constexpr u32 lis(u32 rd, u32 simm) { return addis(rd, 0, simm); }
constexpr u32 ori(u32 ra, u32 rs, u32 uimm) { return D(24, rs, ra, uimm); }
constexpr u32 add(u32 rd, u32 ra, u32 rb, u32 rc = 0) { return X(31, rd, ra, rb, 266, rc); }
constexpr u32 cmpwi(u32 crf, u32 ra, u32 simm) { return D(11, crf << 2, ra, simm); }
constexpr u32 cmpw(u32 crf, u32 ra, u32 rb) { return X(31, crf << 2, ra, rb, 0); }
constexpr u32 rlwnm(u32 ra, u32 rs, u32 rb, u32 mb, u32 me, u32 rc = 0) { return M(23, rs, ra, rb, mb, me, rc); }
constexpr u32 tw(u32 to, u32 ra, u32 rb) { return X(31, to, ra, rb, 4); }
constexpr u32 twi(u32 to, u32 ra, u32 simm) { return D(3, to, ra, simm); }

// --- branch / CR ------------------------------------------------------------
constexpr u32 b(int32_t disp, bool aa = false, bool lk = false)
{
    return (18u << 26) | (static_cast<u32>(disp) & 0x03FFFFFCu) | (aa ? 2u : 0u) | (lk ? 1u : 0u);
}
constexpr u32 bc(u32 bo, u32 bi, int32_t disp, bool aa = false, bool lk = false)
{
    return (16u << 26) | (bo << 21) | (bi << 16) | (static_cast<u32>(disp) & 0xFFFCu) |
           (aa ? 2u : 0u) | (lk ? 1u : 0u);
}
constexpr u32 bdnz(int32_t disp) { return bc(16, 0, disp); }
constexpr u32 beq(int32_t disp, u32 crf = 0) { return bc(12, crf * 4 + 2, disp); }
constexpr u32 bne(int32_t disp, u32 crf = 0) { return bc(4, crf * 4 + 2, disp); }
constexpr u32 bclr(u32 bo, u32 bi, bool lk = false) { return X(19, bo, bi, 0, 16, lk ? 1 : 0); }
constexpr u32 blr() { return bclr(20, 0); }
constexpr u32 blrl() { return bclr(20, 0, true); }
constexpr u32 bctr() { return X(19, 20, 0, 0, 528); }
constexpr u32 crop(u32 xo, u32 bt, u32 ba, u32 bb) { return X(19, bt, ba, bb, xo); }
constexpr u32 mcrf(u32 crfd, u32 crfs) { return X(19, crfd << 2, crfs << 2, 0, 0); }
constexpr u32 mtcrf(u32 crm, u32 rs) { return (31u << 26) | (rs << 21) | (crm << 12) | (144u << 1); }
constexpr u32 mfcr(u32 rd) { return X(31, rd, 0, 0, 19); }
constexpr u32 mcrxr(u32 crfd) { return X(31, crfd << 2, 0, 0, 512); }

// --- load/store --------------------------------------------------------------
constexpr u32 lwz(u32 rd, u32 d, u32 ra) { return D(32, rd, ra, d); }
constexpr u32 lwzu(u32 rd, u32 d, u32 ra) { return D(33, rd, ra, d); }
constexpr u32 lbz(u32 rd, u32 d, u32 ra) { return D(34, rd, ra, d); }
constexpr u32 stw(u32 rs, u32 d, u32 ra) { return D(36, rs, ra, d); }
constexpr u32 stwu(u32 rs, u32 d, u32 ra) { return D(37, rs, ra, d); }
constexpr u32 stb(u32 rs, u32 d, u32 ra) { return D(38, rs, ra, d); }
constexpr u32 lhz(u32 rd, u32 d, u32 ra) { return D(40, rd, ra, d); }
constexpr u32 lha(u32 rd, u32 d, u32 ra) { return D(42, rd, ra, d); }
constexpr u32 sth(u32 rs, u32 d, u32 ra) { return D(44, rs, ra, d); }
constexpr u32 lmw(u32 rd, u32 d, u32 ra) { return D(46, rd, ra, d); }
constexpr u32 stmw(u32 rs, u32 d, u32 ra) { return D(47, rs, ra, d); }
constexpr u32 lwzx(u32 rd, u32 ra, u32 rb) { return X(31, rd, ra, rb, 23); }
constexpr u32 lbzux(u32 rd, u32 ra, u32 rb) { return X(31, rd, ra, rb, 119); }
constexpr u32 lwbrx(u32 rd, u32 ra, u32 rb) { return X(31, rd, ra, rb, 534); }
constexpr u32 stwbrx(u32 rs, u32 ra, u32 rb) { return X(31, rs, ra, rb, 662); }
constexpr u32 lhbrx(u32 rd, u32 ra, u32 rb) { return X(31, rd, ra, rb, 790); }
constexpr u32 lswi(u32 rd, u32 ra, u32 nb) { return X(31, rd, ra, nb, 597); }
constexpr u32 stswi(u32 rs, u32 ra, u32 nb) { return X(31, rs, ra, nb, 725); }
constexpr u32 lwarx(u32 rd, u32 ra, u32 rb) { return X(31, rd, ra, rb, 20); }
constexpr u32 stwcx(u32 rs, u32 ra, u32 rb) { return X(31, rs, ra, rb, 150, 1); }
constexpr u32 dcbz(u32 ra, u32 rb) { return X(31, 0, ra, rb, 1014); }

// --- system ------------------------------------------------------------------
constexpr u32 sc() { return (17u << 26) | 2u; }
constexpr u32 rfi() { return X(19, 0, 0, 0, 50); }
constexpr u32 mfmsr(u32 rd) { return X(31, rd, 0, 0, 83); }
constexpr u32 mtmsr(u32 rs) { return X(31, rs, 0, 0, 146); }
constexpr u32 isync() { return X(19, 0, 0, 0, 150); }
constexpr u32 icbi(u32 ra, u32 rb) { return X(31, 0, ra, rb, 982); }
constexpr u32 mfspr(u32 rd, u32 spr) { return (31u << 26) | (rd << 21) | (spr_field(spr) << 11) | (339u << 1); }
constexpr u32 mtspr(u32 spr, u32 rs) { return (31u << 26) | (rs << 21) | (spr_field(spr) << 11) | (467u << 1); }
constexpr u32 mftb(u32 rd, u32 tbr) { return (31u << 26) | (rd << 21) | (spr_field(tbr) << 11) | (371u << 1); }
constexpr u32 mtsr(u32 sr, u32 rs) { return (31u << 26) | (rs << 21) | (sr << 16) | (210u << 1); }
constexpr u32 mfsr(u32 rd, u32 sr) { return (31u << 26) | (rd << 21) | (sr << 16) | (595u << 1); }
// --- floating point ----------------------------------------------------------
constexpr u32 A(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo, u32 rc = 0)
{
    return (op << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1) | rc;
}
constexpr u32 lfs(u32 frd, u32 d, u32 ra) { return D(48, frd, ra, d); }
constexpr u32 lfsu(u32 frd, u32 d, u32 ra) { return D(49, frd, ra, d); }
constexpr u32 lfd(u32 frd, u32 d, u32 ra) { return D(50, frd, ra, d); }
constexpr u32 stfs(u32 frs, u32 d, u32 ra) { return D(52, frs, ra, d); }
constexpr u32 stfd(u32 frs, u32 d, u32 ra) { return D(54, frs, ra, d); }
constexpr u32 stfdu(u32 frs, u32 d, u32 ra) { return D(55, frs, ra, d); }
constexpr u32 lfdx(u32 frd, u32 ra, u32 rb) { return X(31, frd, ra, rb, 599); }
constexpr u32 stfiwx(u32 frs, u32 ra, u32 rb) { return X(31, frs, ra, rb, 983); }
constexpr u32 fadd(u32 d, u32 a, u32 b, u32 rc = 0) { return A(63, d, a, b, 0, 21, rc); }
constexpr u32 fdiv(u32 d, u32 a, u32 b, u32 rc = 0) { return A(63, d, a, b, 0, 18, rc); }
constexpr u32 fmadd(u32 d, u32 a, u32 c, u32 b) { return A(63, d, a, b, c, 29); }
constexpr u32 fnmadd(u32 d, u32 a, u32 c, u32 b) { return A(63, d, a, b, c, 31); }
constexpr u32 fsel(u32 d, u32 a, u32 c, u32 b) { return A(63, d, a, b, c, 23); }
constexpr u32 fsqrt(u32 d, u32 b) { return A(63, d, 0, b, 0, 22); }
constexpr u32 fres(u32 d, u32 b) { return A(59, d, 0, b, 0, 24); }
constexpr u32 frsqrte(u32 d, u32 b) { return A(63, d, 0, b, 0, 26); }
constexpr u32 fadds(u32 d, u32 a, u32 b) { return A(59, d, a, b, 0, 21); }
constexpr u32 fcmpu(u32 crf, u32 a, u32 b) { return X(63, crf << 2, a, b, 0); }
constexpr u32 fcmpo(u32 crf, u32 a, u32 b) { return X(63, crf << 2, a, b, 32); }
constexpr u32 frsp(u32 d, u32 b) { return X(63, d, 0, b, 12); }
constexpr u32 fctiw(u32 d, u32 b) { return X(63, d, 0, b, 14); }
constexpr u32 fctiwz(u32 d, u32 b) { return X(63, d, 0, b, 15); }
constexpr u32 fmr(u32 d, u32 b) { return X(63, d, 0, b, 72); }
constexpr u32 fneg(u32 d, u32 b) { return X(63, d, 0, b, 40); }
constexpr u32 fabs_(u32 d, u32 b) { return X(63, d, 0, b, 264); }
constexpr u32 fnabs(u32 d, u32 b) { return X(63, d, 0, b, 136); }
constexpr u32 mffs(u32 d, u32 rc = 0) { return X(63, d, 0, 0, 583, rc); }
constexpr u32 mtfsf(u32 fm, u32 b) { return (63u << 26) | (fm << 17) | (b << 11) | (711u << 1); }
constexpr u32 mtfsfi(u32 crf, u32 imm) { return (63u << 26) | (crf << 23) | (imm << 12) | (134u << 1); }
constexpr u32 mtfsb0(u32 bit) { return X(63, bit, 0, 0, 70); }
constexpr u32 mtfsb1(u32 bit) { return X(63, bit, 0, 0, 38); }
constexpr u32 mcrfs(u32 crfd, u32 crfs) { return X(63, crfd << 2, crfs << 2, 0, 64); }

} // namespace asm_ppc

#endif // G3_TESTS_UNIT_CPU_PPC_ASM_H
