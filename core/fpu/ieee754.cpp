// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/fpu/ieee754.cpp — software IEEE 754 arithmetic (see ieee754.h).
//
// Every operation computes an exact result, or one whose discarded low bits
// are "jammed" into a sticky least significant bit with plenty of guard bits
// to spare, and then rounds ONCE in round_pack().

#include "core/fpu/ieee754.h"

namespace g3::fp {

namespace {

using u128 = unsigned __int128;

struct Format {
    int m;       // stored fraction bits
    int bias;
    int emin;    // smallest normal exponent (unbiased)
    int emax;    // largest exponent (unbiased)
    int adjust;  // exponent adjustment for trap-enabled overflow/underflow
    int sign_shift;
};
constexpr Format kDouble{52, 1023, -1022, 1023, 1536, 63};
constexpr Format kSingle{23, 127, -126, 127, 192, 31};

int msb128(u128 x)
{
    const u64 hi = static_cast<u64>(x >> 64);
    if (hi)
        return 127 - __builtin_clzll(hi);
    return 63 - __builtin_clzll(static_cast<u64>(x));
}

u128 shift_right_jam(u128 x, int d)
{
    if (d <= 0)
        return x;
    if (d >= 128)
        return x != 0 ? 1 : 0;
    const u128 lost = x << (128 - d);
    return (x >> d) | (lost != 0 ? 1 : 0);
}

enum class Cls { Zero, Finite, Inf };

struct Unpacked {
    bool sign;
    Cls cls;
    int exp;   // value = sig * 2^exp
    u64 sig;   // finite: normalised, bit 52 set
};

Unpacked unpack(u64 a)
{
    Unpacked u{};
    u.sign = (a >> 63) != 0;
    const int e = static_cast<int>((a >> 52) & 0x7FF);
    const u64 f = a & ((1ull << 52) - 1);
    if (e == 0x7FF) {
        u.cls = Cls::Inf;  // NaNs are filtered out by callers
    } else if (e == 0) {
        if (f == 0) {
            u.cls = Cls::Zero;
        } else {
            const int shift = __builtin_clzll(f) - 11;  // msb -> bit 52
            u.cls = Cls::Finite;
            u.sig = f << shift;
            u.exp = -1074 - shift;
        }
    } else {
        u.cls = Cls::Finite;
        u.sig = f | (1ull << 52);
        u.exp = e - 1075;
    }
    return u;
}

enum class Rem { Exact, Below, Half, Above };

// Splits sig into a quotient q = sig >> shift and classifies the remainder.
void split(u128 sig, int shift, u128& q, Rem& rc)
{
    if (shift <= 0) {
        q = sig << (-shift);
        rc = Rem::Exact;
        return;
    }
    if (shift > 128) {  // sig < 2^128 <= half: below half
        q = 0;
        rc = sig == 0 ? Rem::Exact : Rem::Below;
        return;
    }
    const u128 mask = shift == 128 ? ~static_cast<u128>(0) : ((static_cast<u128>(1) << shift) - 1);
    const u128 rem = sig & mask;
    const u128 half = static_cast<u128>(1) << (shift - 1);
    q = shift == 128 ? 0 : (sig >> shift);
    rc = rem == 0 ? Rem::Exact : (rem < half ? Rem::Below : (rem == half ? Rem::Half : Rem::Above));
}

bool round_increment(Rounding r, bool sign, u128 q, Rem rc)
{
    switch (r) {
    case Rounding::Nearest: return rc == Rem::Above || (rc == Rem::Half && (q & 1));
    case Rounding::Zero:    return false;
    case Rounding::Up:      return rc != Rem::Exact && !sign;
    case Rounding::Down:    return rc != Rem::Exact && sign;
    }
    return false;
}

// Rounds (-1)^sign * sig * 2^exp (sig != 0) into format F; returns the bit
// pattern in that format.
u64 round_pack(bool sign, int exp, u128 sig, const Format& F, const Env& env, Flags& f)
{
    const int n = msb128(sig);
    int E = exp + n;  // value in [2^E, 2^(E+1))

    bool overflow_adjusted = false;
    if (E > F.emax && env.overflow_adjust) {
        exp -= F.adjust;
        E -= F.adjust;
        overflow_adjusted = true;
    }

    // Tininess (IEEE 754 7.5): before rounding = exponent below emin;
    // after rounding = still below 2^emin after rounding to full precision
    // with an unbounded exponent.
    bool tiny = E < F.emin;
    if (tiny && env.tininess == Tininess::AfterRounding && E == F.emin - 1) {
        u128 qf;
        Rem rcf;
        split(sig, n + 1 - (F.m + 1), qf, rcf);
        qf += round_increment(env.rounding, sign, qf, rcf) ? 1 : 0;
        if (qf == (static_cast<u128>(1) << (F.m + 1)))
            tiny = false;
    }

    bool underflow_adjusted = false;
    if (E < F.emin && env.underflow_adjust) {
        exp += F.adjust;
        E += F.adjust;
        underflow_adjusted = true;
    }

    if (E > F.emax) {
        // Overflow (not trap-adjusted): result depends on the rounding mode.
        f.overflow = true;
        f.inexact = true;
        const u64 inf = static_cast<u64>(2 * F.bias + 1) << F.m;
        const bool to_inf = env.rounding == Rounding::Nearest ||
                            (env.rounding == Rounding::Up && !sign) ||
                            (env.rounding == Rounding::Down && sign);
        // FR on overflow: whether rounding the significand to the target
        // precision (unbounded exponent) increments it. Derived from the
        // hardware vectors (e.g. fadds 0+DBL_MAX: FR=1; fadds
        // FLT_MAX+FLT_MAX and fadd DBL_MAX+DBL_MAX: FR=0).
        {
            u128 qf;
            Rem rcf;
            split(sig, n + 1 - (F.m + 1), qf, rcf);
            f.rounded_up = round_increment(env.rounding, sign, qf, rcf);
        }
        return (to_inf ? inf : inf - 1) | (static_cast<u64>(sign) << F.sign_shift);
    }

    const int keep = E >= F.emin ? F.m + 1 : F.m + 1 - (F.emin - E);
    u128 q;
    Rem rc;
    split(sig, n + 1 - keep, q, rc);
    const bool inexact = rc != Rem::Exact;
    const bool inc = round_increment(env.rounding, sign, q, rc);
    q += inc ? 1 : 0;

    // Normal: q in [2^m, 2^(m+1)] includes the implicit bit, which adds one
    // to the biased exponent; a rounding carry propagates into the exponent.
    // Denormal: exponent field 0 (emin + bias - 1 == 0), and a carry to 2^m
    // correctly produces the smallest normal number.
    const int ebase = E >= F.emin ? E : F.emin;
    u64 bits = (static_cast<u64>(ebase + F.bias - 1) << F.m) + static_cast<u64>(q);

    if ((bits >> F.m) >= static_cast<u64>(2 * F.bias + 1)) {
        // Rounding carried past the largest exponent.
        if (env.overflow_adjust && !overflow_adjusted) {
            return round_pack(sign, exp - F.adjust, sig, F, env, f);
        }
        f.overflow = true;
        f.inexact = true;
        const u64 inf = static_cast<u64>(2 * F.bias + 1) << F.m;
        const bool to_inf = env.rounding == Rounding::Nearest ||
                            (env.rounding == Rounding::Up && !sign) ||
                            (env.rounding == Rounding::Down && sign);
        f.rounded_up = true;  // overflow caused by the rounding increment
        return (to_inf ? inf : inf - 1) | (static_cast<u64>(sign) << F.sign_shift);
    }

    if (overflow_adjusted)
        f.overflow = true;
    if (underflow_adjusted)
        f.underflow = f.underflow || tiny;
    else if (tiny && inexact)
        f.underflow = true;
    f.inexact = f.inexact || inexact;
    f.rounded_up = f.rounded_up || inc;
    return bits | (static_cast<u64>(sign) << F.sign_shift);
}

u64 finish(bool sign, int exp, u128 sig, Precision p, const Env& env, Flags& f)
{
    if (p == Precision::Double)
        return round_pack(sign, exp, sig, kDouble, env, f);
    return single_to_double_bits(static_cast<u32>(round_pack(sign, exp, sig, kSingle, env, f)));
}

u64 make_zero(bool sign) { return sign ? kSignBit : 0; }
u64 make_inf(bool sign) { return 0x7FF0000000000000ull | (sign ? kSignBit : 0); }

// Sign of an exact zero sum of operands with different signs (IEEE 754 6.3).
bool zero_sum_sign(Rounding r) { return r == Rounding::Down; }

// X and Y are finite with msb at most bit 125.
u64 add_core(bool sx, int ex, u128 X, bool sy, int ey, u128 Y, Precision p, const Env& env,
             Flags& f)
{
    if (ey > ex || (ey == ex && Y > X)) {
        bool ts = sx; sx = sy; sy = ts;
        int te = ex; ex = ey; ey = te;
        u128 tv = X; X = Y; Y = tv;
    }
    // Now |X| >= |Y| in exponent order. Align Y to X's exponent.
    // X's exponent is the larger, so Y loses low bits into the jam bit.
    Y = shift_right_jam(Y, ex - ey);
    if (sx == sy)
        return finish(sx, ex, X + Y, p, env, f);
    if (X == Y)
        return make_zero(zero_sum_sign(env.rounding));
    if (X > Y)
        return finish(sx, ex, X - Y, p, env, f);
    return finish(sy, ex, Y - X, p, env, f);
}

u64 add_impl(u64 a, u64 b, bool negate_b, Precision p, const Env& env, Flags& f)
{
    Unpacked A = unpack(a), B = unpack(b);
    if (negate_b)
        B.sign = !B.sign;

    if (A.cls == Cls::Inf || B.cls == Cls::Inf) {
        if (A.cls == Cls::Inf && B.cls == Cls::Inf && A.sign != B.sign) {
            f.invalid_isi = true;
            return kDefaultNaN;
        }
        return make_inf(A.cls == Cls::Inf ? A.sign : B.sign);
    }
    if (A.cls == Cls::Zero && B.cls == Cls::Zero)
        return make_zero(A.sign == B.sign ? A.sign : zero_sum_sign(env.rounding));
    if (A.cls == Cls::Zero)
        return finish(B.sign, B.exp, B.sig, p, env, f);
    if (B.cls == Cls::Zero)
        return finish(A.sign, A.exp, A.sig, p, env, f);

    return add_core(A.sign, A.exp - 64, static_cast<u128>(A.sig) << 64, B.sign, B.exp - 64,
                    static_cast<u128>(B.sig) << 64, p, env, f);
}

u128 isqrt_jam(u128 n)
{
    // Bitwise integer square root; returns floor(sqrt(n)) with the low bit
    // jammed if the remainder is non-zero.
    u128 res = 0;
    u128 bit = static_cast<u128>(1) << 126;
    while (bit > n)
        bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res | (n != 0 ? 1 : 0);
}

} // namespace

u64 add(u64 a, u64 b, Precision p, const Env& env, Flags& f)
{
    return add_impl(a, b, false, p, env, f);
}

u64 sub(u64 a, u64 b, Precision p, const Env& env, Flags& f)
{
    return add_impl(a, b, true, p, env, f);
}

u64 mul(u64 a, u64 b, Precision p, const Env& env, Flags& f)
{
    const Unpacked A = unpack(a), B = unpack(b);
    const bool sign = A.sign != B.sign;
    if (A.cls == Cls::Inf || B.cls == Cls::Inf) {
        if (A.cls == Cls::Zero || B.cls == Cls::Zero) {
            f.invalid_imz = true;
            return kDefaultNaN;
        }
        return make_inf(sign);
    }
    if (A.cls == Cls::Zero || B.cls == Cls::Zero)
        return make_zero(sign);
    return finish(sign, A.exp + B.exp, static_cast<u128>(A.sig) * B.sig, p, env, f);
}

u64 div(u64 a, u64 b, Precision p, const Env& env, Flags& f)
{
    const Unpacked A = unpack(a), B = unpack(b);
    const bool sign = A.sign != B.sign;
    if (A.cls == Cls::Inf) {
        if (B.cls == Cls::Inf) {
            f.invalid_idi = true;
            return kDefaultNaN;
        }
        return make_inf(sign);
    }
    if (B.cls == Cls::Inf)
        return make_zero(sign);
    if (B.cls == Cls::Zero) {
        if (A.cls == Cls::Zero) {
            f.invalid_zdz = true;
            return kDefaultNaN;
        }
        f.divide_by_zero = true;
        return make_inf(sign);
    }
    if (A.cls == Cls::Zero)
        return make_zero(sign);

    // Quotient with >= 74 bits; the remainder is jammed into the low bit.
    const u128 n = static_cast<u128>(A.sig) << 75;
    u128 q = n / B.sig;
    if (n % B.sig)
        q |= 1;
    return finish(sign, A.exp - B.exp - 75, q, p, env, f);
}

u64 fma(u64 a, u64 c, u64 b, bool negate_addend, Precision p, const Env& env, Flags& f)
{
    const Unpacked A = unpack(a), C = unpack(c);
    Unpacked B = unpack(b);
    if (negate_addend)
        B.sign = !B.sign;
    const bool ps = A.sign != C.sign;

    if (A.cls == Cls::Inf || C.cls == Cls::Inf) {
        if (A.cls == Cls::Zero || C.cls == Cls::Zero) {
            f.invalid_imz = true;
            return kDefaultNaN;
        }
        if (B.cls == Cls::Inf && B.sign != ps) {
            f.invalid_isi = true;
            return kDefaultNaN;
        }
        return make_inf(ps);
    }
    if (B.cls == Cls::Inf)
        return make_inf(B.sign);

    const bool product_zero = A.cls == Cls::Zero || C.cls == Cls::Zero;
    if (product_zero) {
        if (B.cls == Cls::Zero)
            return make_zero(ps == B.sign ? ps : zero_sum_sign(env.rounding));
        return finish(B.sign, B.exp, B.sig, p, env, f);
    }

    u128 P = static_cast<u128>(A.sig) * C.sig;
    int ep = A.exp + C.exp;
    if (B.cls == Cls::Zero)
        return finish(ps, ep, P, p, env, f);

    // Both non-zero: align with msb at bit 125 for headroom.
    const int sp = 125 - msb128(P);
    P <<= sp;
    ep -= sp;
    const u128 Bv = static_cast<u128>(B.sig) << 73;
    const int eb = B.exp - 73;
    return add_core(ps, ep, P, B.sign, eb, Bv, p, env, f);
}

u64 round_to_single(u64 a, const Env& env, Flags& f)
{
    const Unpacked A = unpack(a);
    if (A.cls == Cls::Zero)
        return make_zero(A.sign);
    if (A.cls == Cls::Inf)
        return make_inf(A.sign);
    return finish(A.sign, A.exp, A.sig, Precision::Single, env, f);
}

u64 sqrt(u64 a, Precision p, const Env& env, Flags& f)
{
    const Unpacked A = unpack(a);
    if (A.cls == Cls::Zero)
        return make_zero(A.sign);  // sqrt(-0) = -0
    if (A.sign) {
        f.invalid_sqrt = true;
        return kDefaultNaN;
    }
    if (A.cls == Cls::Inf)
        return make_inf(false);
    // Scale the 53-bit significand to ~124 bits with an even exponent.
    int s = 71;
    if ((A.exp - s) & 1)
        ++s;
    const u128 n = static_cast<u128>(A.sig) << s;
    return finish(false, (A.exp - s) / 2, isqrt_jam(n), p, env, f);
}

u32 to_int32(u64 a, Rounding r, bool& invalid, bool& inexact, bool& rounded_up)
{
    invalid = inexact = rounded_up = false;
    if (is_nan(a)) {
        invalid = true;
        return 0x80000000u;
    }
    const Unpacked A = unpack(a);
    if (A.cls == Cls::Inf) {
        invalid = true;
        return A.sign ? 0x80000000u : 0x7FFFFFFFu;
    }
    if (A.cls == Cls::Zero)
        return 0;

    u128 q;
    Rem rc;
    if (A.exp >= 0) {
        if (A.exp > 40) {  // far beyond the 32-bit range
            invalid = true;
            return A.sign ? 0x80000000u : 0x7FFFFFFFu;
        }
        q = static_cast<u128>(A.sig) << A.exp;
        rc = Rem::Exact;
    } else {
        split(A.sig, -A.exp, q, rc);
    }
    const bool inc = round_increment(r, A.sign, q, rc);
    q += inc ? 1 : 0;

    const u128 limit = A.sign ? 0x80000000u : 0x7FFFFFFFu;
    if (q > limit) {
        invalid = true;
        return A.sign ? 0x80000000u : 0x7FFFFFFFu;
    }
    inexact = rc != Rem::Exact;
    rounded_up = inc;
    const u32 mag = static_cast<u32>(q);
    return A.sign ? (0u - mag) : mag;
}

u64 single_to_double_bits(u32 s)
{
    const u64 sign = static_cast<u64>(s >> 31) << 63;
    const u32 e = (s >> 23) & 0xFF;
    u32 frac = s & 0x7FFFFF;
    if (e == 0xFF)
        return sign | 0x7FF0000000000000ull | (static_cast<u64>(frac) << 29);
    if (e == 0) {
        if (frac == 0)
            return sign;
        const int shift = __builtin_clz(frac) - 8;  // msb -> bit 23
        frac <<= shift;
        const u64 biased = static_cast<u64>(1023 - 126 - shift);
        return sign | (biased << 52) | (static_cast<u64>(frac & 0x7FFFFF) << 29);
    }
    return sign | (static_cast<u64>(e - 127 + 1023) << 52) | (static_cast<u64>(frac) << 29);
}

u32 double_to_single_bits(u64 d)
{
    const u32 exp = static_cast<u32>((d >> 52) & 0x7FF);
    if (exp >= 874 && exp <= 896) {
        // Denormalise (truncating) into the single denormal range.
        const int e = static_cast<int>(exp) - 1023;
        u64 mant = (1ull << 52) | (d & ((1ull << 52) - 1));
        mant >>= (-126 - e);
        return static_cast<u32>((d >> 63) << 31) | static_cast<u32>((mant >> 29) & 0x7FFFFF);
    }
    // WORD[0-1] = frS[0-1], WORD[2-31] = frS[5-34] (PEM stfs). Values with
    // exponents below 874 are architecturally undefined; they use this path.
    return static_cast<u32>(((d >> 32) & 0xC0000000u) | ((d >> 29) & 0x3FFFFFFFu));
}

} // namespace g3::fp
