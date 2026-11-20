// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_fpu.cpp — floating-point instructions and FPSCR
// handling (PEM chapter 3.3, chapter 12; 750 UM chapter 2).
//
// Arithmetic is done by the software IEEE implementation in core/fpu; this
// file applies the PowerPC rules on top: NaN propagation, invalid-operation
// causes, FPSCR status/summary bits, result classes (FPRF), suppression of
// the result for enabled invalid/zero-divide exceptions, CR1 for Rc=1, and
// the FP-enabled program exception.

#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

namespace {

constexpr u64 kQuietBit = 0x0008000000000000ull;
constexpr u64 kDoubleOne = 0x3FF0000000000000ull;

constexpr u32 frc(u32 insn) { return (insn >> 6) & 31; }

// FPRF class of a result. Single-precision results are classified in the
// single format (a single denormal is "denormalized").
u32 classify(u64 v, bool single)
{
    const bool neg = fp::sign_of(v);
    if (fp::is_nan(v))
        return fpscr::kClassQNaN;
    if (fp::is_inf(v))
        return neg ? fpscr::kClassNegInf : fpscr::kClassPosInf;
    if (fp::is_zero(v))
        return neg ? fpscr::kClassNegZero : fpscr::kClassPosZero;
    bool denormal;
    if (single) {
        denormal = ((fp::double_to_single_bits(v) >> 23) & 0xFF) == 0;
    } else {
        denormal = ((v >> 52) & 0x7FF) == 0;
    }
    if (denormal)
        return neg ? fpscr::kClassNegDen : fpscr::kClassPosDen;
    return neg ? fpscr::kClassNegNorm : fpscr::kClassPosNorm;
}

// A NaN result of a single-precision instruction is delivered with the
// fraction truncated to single precision (PEM 3.3.6.1, frsp / "round to
// single" of a NaN).
u64 nan_to_single(u64 v)
{
    return v & ~0x1FFFFFFFull;
}

// Single-precision instructions that use frC (fmuls, fmadds, fmsubs,
// fnmadds, fnmsubs) take frC with its significand rounded to 25 bits
// (24 fraction bits), rounding half up, independent of FPSCR[RN].
//
// Basis: the hardware-derived vector "fmuls 0.25 * 0x3FD6666666666666"
// (tests/vectors/dingusppc, line 1246) is exact on hardware, which requires
// frC to be shortened first; the exact rule used here is the one documented
// for the Gekko, a derivative of the 750 core, whose multiplier takes frC in
// ~27-bit parts. The single vector also fits truncation to 24 bits.
// INFERRED — to be verified on a real 750 (docs/hardware/ppc750-cpu.md).
u64 shorten_frc_for_single(u64 c)
{
    if (fp::is_nan(c) || fp::is_inf(c) || ((c >> 52) & 0x7FF) == 0)
        return c;  // specials and denormals unchanged (not covered by evidence)
    return (c & 0xFFFFFFFFF8000000ull) + (c & 0x0000000008000000ull);
}

} // namespace

bool Ppc750::fp_available()
{
    if (!(s_.msr & msr::FP)) {
        raise_exception(Exception::FpUnavailable, s_.pc);
        return false;
    }
    return true;
}

void Ppc750::fp_update_summary(u32 old)
{
    u32 f = s_.fpscr;
    // FX: set when any exception bit changes from 0 to 1.
    if ((f & ~old) & fpscr::kExceptionBits)
        f |= fpscr::FX;
    f = (f & fpscr::kAllVx) ? (f | fpscr::VX) : (f & ~fpscr::VX);
    const bool fex = ((f & fpscr::VX) && (f & fpscr::VE)) || ((f & fpscr::OX) && (f & fpscr::OE)) ||
                     ((f & fpscr::UX) && (f & fpscr::UE)) || ((f & fpscr::ZX) && (f & fpscr::ZE)) ||
                     ((f & fpscr::XX) && (f & fpscr::XE));
    f = fex ? (f | fpscr::FEX) : (f & ~fpscr::FEX);
    s_.fpscr = f;
}

void Ppc750::fp_finish(u32 insn)
{
    if (field::rc(insn))
        set_cr_field(1, s_.fpscr >> 28);
    // FP-enabled exception: taken if FEX is set and either FE0 or FE1 is set
    // (imprecise modes are treated as precise). SRR0 = this instruction.
    if ((s_.fpscr & fpscr::FEX) && (s_.msr & (msr::FE0 | msr::FE1)))
        raise_exception(Exception::Program, s_.pc, srr1::FP_ENABLED);
}

void Ppc750::fp_commit(u32 insn, u64 result, bool is_nan_result, bool vxsnan, u32 other_vx,
                       const fp::Flags& fl, bool single, bool negate)
{
    const u32 old = s_.fpscr;
    u32 f = old & ~(fpscr::FR | fpscr::FI);

    u32 vx = other_vx;
    if (vxsnan) vx |= fpscr::VXSNAN;
    if (fl.invalid_isi) vx |= fpscr::VXISI;
    if (fl.invalid_idi) vx |= fpscr::VXIDI;
    if (fl.invalid_zdz) vx |= fpscr::VXZDZ;
    if (fl.invalid_imz) vx |= fpscr::VXIMZ;
    if (fl.invalid_sqrt) vx |= fpscr::VXSQRT;
    f |= vx;
    if (fl.divide_by_zero) f |= fpscr::ZX;

    const bool suppress = (vx && (old & fpscr::VE)) || (fl.divide_by_zero && (old & fpscr::ZE));
    if (!suppress) {
        if (fl.overflow) f |= fpscr::OX;
        if (fl.underflow) f |= fpscr::UX;
        if (fl.inexact) f |= fpscr::XX | fpscr::FI;
        if (fl.rounded_up) f |= fpscr::FR;
        // fnmadd/fnmsub negate the rounded result; a NaN keeps its sign.
        const u64 value = (negate && !is_nan_result) ? (result ^ fp::kSignBit) : result;
        s_.fpr[field::rd(insn)] = value;
        f = (f & ~fpscr::FPRF) | classify(value, single);
    }
    s_.fpscr = f;
    fp_update_summary(old);
    fp_finish(insn);
}

// --- arithmetic -------------------------------------------------------------------

void Ppc750::fp_arith(u32 insn, bool single)
{
    const u32 xo = (insn >> 1) & 0x1F;
    const u64 a = s_.fpr[field::ra(insn)];
    const u64 b = s_.fpr[field::rb(insn)];
    u64 c = s_.fpr[frc(insn)];
    if (single)
        c = shorten_frc_for_single(c);

    // Which operands the instruction uses, in PowerPC NaN-precedence order
    // (frA, frB, frC; PEM 3.3.6.1.1).
    bool use_a = true, use_b = true, use_c = false;
    switch (xo) {
    case 18: case 20: case 21: break;               // div, sub, add
    case 25: use_b = false; use_c = true; break;    // mul: frA * frC
    case 28: case 29: case 30: case 31: use_c = true; break;  // fused multiply-add family
    default:
        illegal(insn);
        return;
    }

    fp::Env env;
    env.rounding = static_cast<fp::Rounding>(s_.fpscr & fpscr::RN);
    env.tininess = fp_tininess_;
    env.overflow_adjust = (s_.fpscr & fpscr::OE) != 0;
    env.underflow_adjust = (s_.fpscr & fpscr::UE) != 0;
    fp::Flags fl;

    const bool vxsnan = (use_a && fp::is_snan(a)) || (use_b && fp::is_snan(b)) ||
                        (use_c && fp::is_snan(c));
    const u64 nan_src = (use_a && fp::is_nan(a)) ? a
                      : (use_b && fp::is_nan(b)) ? b
                      : (use_c && fp::is_nan(c)) ? c
                                                 : 0;
    const bool negate = xo == 30 || xo == 31;  // fnmsub, fnmadd

    if (nan_src) {
        u64 r = nan_src | kQuietBit;
        if (single)
            r = nan_to_single(r);
        fp_commit(insn, r, true, vxsnan, 0, fl, single, negate);
        return;
    }

    const fp::Precision p = single ? fp::Precision::Single : fp::Precision::Double;
    u64 r;
    switch (xo) {
    case 18: r = fp::div(a, b, p, env, fl); break;
    case 20: r = fp::sub(a, b, p, env, fl); break;
    case 21: r = fp::add(a, b, p, env, fl); break;
    case 25: r = fp::mul(a, c, p, env, fl); break;
    case 28: r = fp::fma(a, c, b, true, p, env, fl); break;   // fmsub:  a*c - b
    case 29: r = fp::fma(a, c, b, false, p, env, fl); break;  // fmadd:  a*c + b
    case 30: r = fp::fma(a, c, b, true, p, env, fl); break;   // fnmsub: -(a*c - b)
    case 31: r = fp::fma(a, c, b, false, p, env, fl); break;  // fnmadd: -(a*c + b)
    default: return;
    }
    fp_commit(insn, r, fp::is_nan(r), false, 0, fl, single, negate);
}

void Ppc750::fp_round_to_single(u32 insn)
{
    const u64 b = s_.fpr[field::rb(insn)];
    fp::Flags fl;
    if (fp::is_nan(b)) {
        fp_commit(insn, nan_to_single(b | kQuietBit), true, fp::is_snan(b), 0, fl, true, false);
        return;
    }
    fp::Env env;
    env.rounding = static_cast<fp::Rounding>(s_.fpscr & fpscr::RN);
    env.tininess = fp_tininess_;
    env.overflow_adjust = (s_.fpscr & fpscr::OE) != 0;
    env.underflow_adjust = (s_.fpscr & fpscr::UE) != 0;
    const u64 r = fp::round_to_single(b, env, fl);
    fp_commit(insn, r, false, false, 0, fl, true, false);
}

void Ppc750::fp_estimate(u32 insn, bool reciprocal_sqrt)
{
    // fres / frsqrte. The 750 returns hardware *estimates* (fres: within
    // 1/4096; frsqrte: within 1/32 of the exact value). We return the
    // correctly rounded value, which is within those bounds but may differ
    // in low-order bits from real hardware (docs/hardware/ppc750-cpu.md).
    const u64 b = s_.fpr[field::rb(insn)];
    fp::Flags fl;
    if (fp::is_nan(b)) {
        u64 r = b | kQuietBit;
        if (!reciprocal_sqrt)
            r = nan_to_single(r);
        fp_commit(insn, r, true, fp::is_snan(b), 0, fl, !reciprocal_sqrt, false);
        return;
    }
    fp::Env env;
    env.rounding = static_cast<fp::Rounding>(s_.fpscr & fpscr::RN);
    env.tininess = fp_tininess_;
    u64 r;
    if (reciprocal_sqrt) {
        if (fp::sign_of(b) && !fp::is_zero(b)) {
            fl.invalid_sqrt = true;
            r = fp::kDefaultNaN;
        } else if (fp::is_zero(b)) {
            fl.divide_by_zero = true;
            r = 0x7FF0000000000000ull | (b & fp::kSignBit);
        } else {
            fp::Flags tmp;
            const u64 root = fp::sqrt(b, fp::Precision::Double, env, tmp);
            r = fp::div(kDoubleOne, root, fp::Precision::Double, env, fl);
        }
    } else {
        r = fp::div(kDoubleOne, b, fp::Precision::Single, env, fl);
    }
    fp_commit(insn, r, fp::is_nan(r), false, 0, fl, !reciprocal_sqrt, false);
}

void Ppc750::fp_convert_to_int(u32 insn, bool toward_zero)
{
    const u64 b = s_.fpr[field::rb(insn)];
    const fp::Rounding r =
        toward_zero ? fp::Rounding::Zero : static_cast<fp::Rounding>(s_.fpscr & fpscr::RN);
    bool invalid, inexact, up;
    const u32 v = fp::to_int32(b, r, invalid, inexact, up);

    const u32 old = s_.fpscr;
    u32 f = old & ~(fpscr::FR | fpscr::FI);
    if (invalid) {
        f |= fpscr::VXCVI;
        if (fp::is_snan(b))
            f |= fpscr::VXSNAN;
    }
    if (!(invalid && (old & fpscr::VE))) {
        // Low word = the integer. The high word is architecturally
        // undefined; 0 here (real 750 value to be verified).
        s_.fpr[field::rd(insn)] = v;
        if (inexact) f |= fpscr::XX | fpscr::FI;
        if (up) f |= fpscr::FR;
        // FPRF is undefined after fctiw[z]; left unchanged.
    }
    s_.fpscr = f;
    fp_update_summary(old);
    fp_finish(insn);
}

void Ppc750::fp_compare(u32 insn, bool ordered)
{
    const u64 a = s_.fpr[field::ra(insn)];
    const u64 b = s_.fpr[field::rb(insn)];
    const u32 old = s_.fpscr;
    u32 f = old;

    u32 cc;
    if (fp::is_nan(a) || fp::is_nan(b)) {
        cc = 1;  // FU: unordered
        const bool snan = fp::is_snan(a) || fp::is_snan(b);
        if (snan)
            f |= fpscr::VXSNAN;
        // fcmpo: VXVC for any NaN, except an SNaN with VE = 1 (PEM fcmpo).
        if (ordered && (!snan || !(old & fpscr::VE)))
            f |= fpscr::VXVC;
    } else {
        // IEEE ordering (-0 == +0): map each value to an integer that
        // sorts the same way (sign-magnitude -> two's complement).
        auto key = [](u64 v) -> s64 {
            const s64 mag = static_cast<s64>(v & ~fp::kSignBit);
            return fp::sign_of(v) ? -mag : mag;
        };
        const s64 ka = key(a), kb = key(b);
        const bool lt = ka < kb, gt = ka > kb;
        cc = lt ? 8 : (gt ? 4 : 2);
    }
    f = (f & ~fpscr::FPCC) | (cc << 12);
    s_.fpscr = f;
    set_cr_field(field::crfd(insn), cc);
    fp_update_summary(old);
    // fcmpu/fcmpo have no Rc bit; FP-enabled exception check still applies.
    if ((s_.fpscr & fpscr::FEX) && (s_.msr & (msr::FE0 | msr::FE1)))
        raise_exception(Exception::Program, s_.pc, srr1::FP_ENABLED);
}

void Ppc750::fp_move(u32 insn, u32 xo)
{
    const u64 b = s_.fpr[field::rb(insn)];
    u64 r;
    switch (xo) {
    case 72:  r = b; break;                       // fmr
    case 40:  r = b ^ fp::kSignBit; break;        // fneg
    case 264: r = b & ~fp::kSignBit; break;       // fabs
    case 136: r = b | fp::kSignBit; break;        // fnabs
    default:
        illegal(insn);
        return;
    }
    s_.fpr[field::rd(insn)] = r;  // no FPSCR change
    if (field::rc(insn))
        set_cr_field(1, s_.fpscr >> 28);
}

void Ppc750::fp_fpscr_op(u32 insn, u32 xo)
{
    const u32 old = s_.fpscr;
    switch (xo) {
    case 583:  // mffs: frD[32-63] = FPSCR (high word undefined; 0 here)
        s_.fpr[field::rd(insn)] = s_.fpscr;
        if (field::rc(insn))
            set_cr_field(1, s_.fpscr >> 28);
        return;
    case 64: {  // mcrfs: copy a field to CR, then clear its exception bits
        const u32 shift = 28 - 4 * field::crfs(insn);
        set_cr_field(field::crfd(insn), (s_.fpscr >> shift) & 0xF);
        const u32 clearable = (fpscr::FX | fpscr::kExceptionBits) & (0xFu << shift);
        s_.fpscr &= ~clearable;
        fp_update_summary(s_.fpscr);  // recompute VX/FEX only; no new FX
        return;
    }
    case 711: {  // mtfsf FM,frB
        const u32 fm = (insn >> 17) & 0xFF;
        u32 mask = 0;
        for (u32 i = 0; i < 8; ++i)
            if (fm & (0x80u >> i))
                mask |= 0xF0000000u >> (4 * i);
        mask &= ~(fpscr::FEX | fpscr::VX);  // cannot be set explicitly
        const u32 v = static_cast<u32>(s_.fpr[field::rb(insn)]);
        s_.fpscr = (s_.fpscr & ~mask) | (v & mask);
        break;
    }
    case 134: {  // mtfsfi crfD,IMM
        const u32 shift = 28 - 4 * field::crfd(insn);
        u32 mask = (0xFu << shift) & ~(fpscr::FEX | fpscr::VX);
        const u32 imm = (insn >> 12) & 0xF;
        s_.fpscr = (s_.fpscr & ~mask) | ((imm << shift) & mask);
        break;
    }
    case 38:    // mtfsb1 crbD
    case 70: {  // mtfsb0 crbD
        const u32 bit = 0x80000000u >> field::rd(insn);
        if (bit != fpscr::FEX && bit != fpscr::VX)
            s_.fpscr = xo == 38 ? (s_.fpscr | bit) : (s_.fpscr & ~bit);
        break;
    }
    default:
        illegal(insn);
        return;
    }
    // FX is only set by these if an exception bit was newly set (mtfsf/
    // mtfsfi can also set or clear FX itself).
    const u32 explicit_fx = s_.fpscr & fpscr::FX;
    fp_update_summary(old);
    if (xo == 711 || xo == 134) {
        // Explicit FX value written by mtfsf/mtfsfi wins unless an exception
        // bit transitioned to 1.
        if (!((s_.fpscr & ~old) & fpscr::kExceptionBits))
            s_.fpscr = (s_.fpscr & ~fpscr::FX) | explicit_fx;
    }
    fp_finish(insn);
}

// --- opcode groups ----------------------------------------------------------------

void Ppc750::execute_op59(u32 insn)
{
    switch ((insn >> 1) & 0x1F) {
    case 18: case 20: case 21: case 25: case 28: case 29: case 30: case 31:
        fp_arith(insn, true);
        break;
    case 24:  // fres
        fp_estimate(insn, false);
        break;
    default:  // includes fsqrts (22), which the 750 does not implement
        illegal(insn);
        break;
    }
}

void Ppc750::execute_op63(u32 insn)
{
    const u32 xo5 = (insn >> 1) & 0x1F;
    if (xo5 >= 16) {  // A-form
        switch (xo5) {
        case 18: case 20: case 21: case 25: case 28: case 29: case 30: case 31:
            fp_arith(insn, false);
            return;
        case 23: {  // fsel: frA >= 0.0 ? frC : frB (NaN selects frB); no FPSCR change
            const u64 a = s_.fpr[field::ra(insn)];
            const bool ge = !fp::is_nan(a) && (!fp::sign_of(a) || fp::is_zero(a));
            s_.fpr[field::rd(insn)] = ge ? s_.fpr[frc(insn)] : s_.fpr[field::rb(insn)];
            if (field::rc(insn))
                set_cr_field(1, s_.fpscr >> 28);
            return;
        }
        case 26:  // frsqrte
            fp_estimate(insn, true);
            return;
        default:  // includes fsqrt (22): not on the 750
            illegal(insn);
            return;
        }
    }
    const u32 xo = field::xo10(insn);
    switch (xo) {
    case 0:  fp_compare(insn, false); break;          // fcmpu
    case 32: fp_compare(insn, true); break;           // fcmpo
    case 12: fp_round_to_single(insn); break;         // frsp
    case 14: fp_convert_to_int(insn, false); break;   // fctiw
    case 15: fp_convert_to_int(insn, true); break;    // fctiwz
    case 40: case 72: case 136: case 264:             // fneg fmr fnabs fabs
        fp_move(insn, xo);
        break;
    case 38: case 64: case 70: case 134: case 583: case 711:
        fp_fpscr_op(insn, xo);
        break;
    default:
        illegal(insn);
        break;
    }
}

// --- loads and stores --------------------------------------------------------------

bool Ppc750::mem_read64(u32 a, u64& v)
{
    u32 hi, lo;
    if (!mem_read32(a, hi) || !mem_read32(a + 4, lo))
        return false;
    v = (static_cast<u64>(hi) << 32) | lo;
    return true;
}

bool Ppc750::mem_write64(u32 a, u64 v)
{
    return mem_write32(a, static_cast<u32>(v >> 32)) && mem_write32(a + 4, static_cast<u32>(v));
}

namespace {
struct FpLs {
    bool dbl, store, update;
};
} // namespace

bool Ppc750::fp_load_store_d(u32 insn, u32 opcode)
{
    // 48 lfs 49 lfsu 50 lfd 51 lfdu 52 stfs 53 stfsu 54 stfd 55 stfdu
    const u32 k = opcode - 48;
    const FpLs f{(k & 2) != 0, k >= 4, (k & 1) != 0};
    const u32 ea = ea_d(insn);
    const u32 fr = field::rd(insn);
    if (f.store) {
        const bool ok = f.dbl ? mem_write64(ea, s_.fpr[fr])
                              : mem_write32(ea, fp::double_to_single_bits(s_.fpr[fr]));
        if (!ok)
            return true;
    } else {
        if (f.dbl) {
            u64 v;
            if (!mem_read64(ea, v))
                return true;
            s_.fpr[fr] = v;
        } else {
            u32 v;
            if (!mem_read32(ea, v))
                return true;
            s_.fpr[fr] = fp::single_to_double_bits(v);
        }
    }
    if (f.update)
        s_.gpr[field::ra(insn)] = ea;
    return true;
}

bool Ppc750::fp_load_store_x(u32 insn, u32 xo)
{
    const u32 ea = ea_x(insn);
    const u32 fr = field::rd(insn);
    if (xo == 983) {  // stfiwx: low word of frS, no conversion
        mem_write32(ea, static_cast<u32>(s_.fpr[fr]));
        return true;
    }
    FpLs f;
    switch (xo) {
    case 535: f = {false, false, false}; break;  // lfsx
    case 567: f = {false, false, true}; break;   // lfsux
    case 599: f = {true, false, false}; break;   // lfdx
    case 631: f = {true, false, true}; break;    // lfdux
    case 663: f = {false, true, false}; break;   // stfsx
    case 695: f = {false, true, true}; break;    // stfsux
    case 727: f = {true, true, false}; break;    // stfdx
    case 759: f = {true, true, true}; break;     // stfdux
    default:
        illegal(insn);
        return true;
    }
    if (f.store) {
        const bool ok = f.dbl ? mem_write64(ea, s_.fpr[fr])
                              : mem_write32(ea, fp::double_to_single_bits(s_.fpr[fr]));
        if (!ok)
            return true;
    } else {
        if (f.dbl) {
            u64 v;
            if (!mem_read64(ea, v))
                return true;
            s_.fpr[fr] = v;
        } else {
            u32 v;
            if (!mem_read32(ea, v))
                return true;
            s_.fpr[fr] = fp::single_to_double_bits(v);
        }
    }
    if (f.update)
        s_.gpr[field::ra(insn)] = ea;
    return true;
}

} // namespace g3::ppc
