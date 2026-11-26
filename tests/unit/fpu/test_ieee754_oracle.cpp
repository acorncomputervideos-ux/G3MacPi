// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compares core/fpu/ieee754 with the host's hardware FPU (the oracle) on
// random operands biased towards hard cases, in all four rounding modes.
// Results must match bit-for-bit (NaNs: NaN-ness only, since the default
// NaN differs between hosts) and the IEEE status flags must match.
//
// The oracle detects tininess AFTER rounding on x86-64 (SSE); on AArch64
// hosts BEFORE rounding. The environment is set accordingly.
//
// This file is compiled with -frounding-math (see CMakeLists.txt).

#include "core/fpu/ieee754.h"
#include "tests/framework/g3test.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace g3;

namespace {

u64 bits_of(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }
double double_of(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
u32 bits_of(float f) { u32 b; std::memcpy(&b, &f, 4); return b; }
float float_of(u32 b) { float f; std::memcpy(&f, &b, 4); return f; }

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u64 next()
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
};

// Random binary64 operand, biased to interesting exponents.
u64 random_double(Rng& r)
{
    const u64 x = r.next();
    const u64 sign = x & fp::kSignBit;
    const u64 frac = r.next() & ((1ull << 52) - 1);
    u64 exp;
    switch (x % 12) {
    case 0: exp = 0; break;                                   // denormal / zero
    case 1: exp = 1 + (x >> 8) % 3; break;                    // smallest normals
    case 2: exp = 2046 - (x >> 8) % 3; break;                 // largest
    case 3: exp = 0x7FF; return sign | (exp << 52);           // infinity
    case 4: exp = 1023 + (x >> 8) % 4; return sign | (exp << 52) | (frac & ~0xFFFFFull);  // short
    case 5: exp = 1023 - 126 - (x >> 8) % 30; break;          // single denormal range
    case 6: exp = 1023 + 126 + (x >> 8) % 3; break;           // single overflow edge
    case 7: return sign;                                      // zero
    default: exp = 1023 + static_cast<s64>((x >> 8) % 120) - 60; break;
    }
    return sign | (exp << 52) | frac;
}

u32 random_float(Rng& r)
{
    const u32 x = static_cast<u32>(r.next());
    const u32 sign = x & 0x80000000u;
    const u32 frac = static_cast<u32>(r.next()) & 0x7FFFFF;
    u32 exp;
    switch (x % 10) {
    case 0: exp = 0; break;
    case 1: exp = 1 + (x >> 8) % 3; break;
    case 2: exp = 254 - (x >> 8) % 3; break;
    case 3: return sign | 0x7F800000u;
    case 4: return sign;
    case 5: exp = 127 + (x >> 8) % 4; return sign | (exp << 23) | (frac & ~0x3FFu);
    default: exp = 127 + static_cast<s32>((x >> 8) % 60) - 30; break;
    }
    return sign | (exp << 23) | frac;
}

constexpr int kModes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};

fp::Env env_for(int i)
{
    fp::Env e;
    e.rounding = static_cast<fp::Rounding>(i);
#if defined(__x86_64__) || defined(__i386__)
    e.tininess = fp::Tininess::AfterRounding;
#else
    e.tininess = fp::Tininess::BeforeRounding;
#endif
    return e;
}

struct HostFlags {
    bool invalid, divzero, overflow, underflow, inexact;
};

HostFlags host_flags()
{
    return {std::fetestexcept(FE_INVALID) != 0, std::fetestexcept(FE_DIVBYZERO) != 0,
            std::fetestexcept(FE_OVERFLOW) != 0, std::fetestexcept(FE_UNDERFLOW) != 0,
            std::fetestexcept(FE_INEXACT) != 0};
}

enum class Op { Add, Sub, Mul, Div, Fma, Fms, Sqrt };
const char* op_name(Op o)
{
    switch (o) {
    case Op::Add: return "add";
    case Op::Sub: return "sub";
    case Op::Mul: return "mul";
    case Op::Div: return "div";
    case Op::Fma: return "fma";
    case Op::Fms: return "fms";
    case Op::Sqrt: return "sqrt";
    }
    return "?";
}

int g_reported = 0;

// Checks one case; returns true if it matches.
bool check(Op op, bool single, int mode, u64 a, u64 b, u64 c)
{
    const fp::Env env = env_for(mode);
    fp::Flags f;
    u64 ours = 0;
    const fp::Precision p = single ? fp::Precision::Single : fp::Precision::Double;
    switch (op) {
    case Op::Add: ours = fp::add(a, b, p, env, f); break;
    case Op::Sub: ours = fp::sub(a, b, p, env, f); break;
    case Op::Mul: ours = fp::mul(a, b, p, env, f); break;
    case Op::Div: ours = fp::div(a, b, p, env, f); break;
    case Op::Fma: ours = fp::fma(a, c, b, false, p, env, f); break;
    case Op::Fms: ours = fp::fma(a, c, b, true, p, env, f); break;
    case Op::Sqrt: ours = fp::sqrt(a, p, env, f); break;
    }

    std::fesetround(kModes[mode]);
    std::feclearexcept(FE_ALL_EXCEPT);
    u64 host;
    if (single) {
        volatile float x = float_of(fp::double_to_single_bits(a));
        volatile float y = float_of(fp::double_to_single_bits(b));
        volatile float z = float_of(fp::double_to_single_bits(c));
        volatile float r = 0;
        switch (op) {
        case Op::Add: r = x + y; break;
        case Op::Sub: r = x - y; break;
        case Op::Mul: r = x * y; break;
        case Op::Div: r = x / y; break;
        case Op::Fma: r = std::fmaf(x, z, y); break;
        case Op::Fms: r = std::fmaf(x, z, -y); break;
        case Op::Sqrt: r = std::sqrt(static_cast<float>(x)); break;
        }
        host = fp::single_to_double_bits(bits_of(static_cast<float>(r)));
    } else {
        volatile double x = double_of(a), y = double_of(b), z = double_of(c);
        volatile double r = 0;
        switch (op) {
        case Op::Add: r = x + y; break;
        case Op::Sub: r = x - y; break;
        case Op::Mul: r = x * y; break;
        case Op::Div: r = x / y; break;
        case Op::Fma: r = std::fma(x, z, y); break;
        case Op::Fms: r = std::fma(x, z, -y); break;
        case Op::Sqrt: r = std::sqrt(static_cast<double>(x)); break;
        }
        host = bits_of(static_cast<double>(r));
    }
    const HostFlags hf = host_flags();
    std::fesetround(FE_TONEAREST);

    const bool value_ok = (fp::is_nan(host) && fp::is_nan(ours)) || host == ours;
    const bool flags_ok = hf.invalid == f.invalid() && hf.divzero == f.divide_by_zero &&
                          hf.overflow == f.overflow && hf.underflow == f.underflow &&
                          hf.inexact == f.inexact;
    if ((!value_ok || !flags_ok) && g_reported++ < 15) {
        std::fprintf(stderr,
                     "  %s%s mode %d a=%016llX b=%016llX c=%016llX: host %016llX "
                     "[I%d Z%d O%d U%d X%d], ours %016llX [I%d Z%d O%d U%d X%d]\n",
                     op_name(op), single ? "s" : "", mode, (unsigned long long)a,
                     (unsigned long long)b, (unsigned long long)c, (unsigned long long)host,
                     hf.invalid, hf.divzero, hf.overflow, hf.underflow, hf.inexact,
                     (unsigned long long)ours, f.invalid(), f.divide_by_zero, f.overflow,
                     f.underflow, f.inexact);
    }
    return value_ok && flags_ok;
}

int run_op(Op op, bool single, int iterations)
{
    Rng rng;
    rng.s ^= static_cast<u64>(op) * 0x1234567ull + (single ? 77 : 0);
    int failures = 0;
    for (int i = 0; i < iterations; ++i) {
        u64 a, b, c;
        if (single) {
            a = fp::single_to_double_bits(random_float(rng));
            b = fp::single_to_double_bits(random_float(rng));
            c = fp::single_to_double_bits(random_float(rng));
        } else {
            a = random_double(rng);
            b = random_double(rng);
            c = random_double(rng);
            // Make exact cancellations and ties likely sometimes.
            if (i % 17 == 0)
                b = a ^ fp::kSignBit;
            if (i % 23 == 0)
                c = 0x3FF0000000000000ull;  // 1.0
        }
        for (int mode = 0; mode < 4; ++mode)
            if (!check(op, single, mode, a, b, c))
                ++failures;
    }
    return failures;
}

constexpr int kIterations = 40000;  // x 4 rounding modes

} // namespace

TEST_CASE("fpu/ieee754/oracle_double")
{
    const Op ops[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Fma, Op::Fms, Op::Sqrt};
    for (Op op : ops) {
        const int failures = run_op(op, false, kIterations);
        if (failures)
            std::fprintf(stderr, "  %s: %d failures\n", op_name(op), failures);
        CHECK_EQ(failures, 0);
    }
}

TEST_CASE("fpu/ieee754/oracle_single")
{
    const Op ops[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Fma, Op::Fms, Op::Sqrt};
    for (Op op : ops) {
        const int failures = run_op(op, true, kIterations);
        if (failures)
            std::fprintf(stderr, "  %ss: %d failures\n", op_name(op), failures);
        CHECK_EQ(failures, 0);
    }
}

TEST_CASE("fpu/ieee754/round_to_single_and_to_int32_oracle")
{
    Rng rng;
    int failures = 0;
    for (int i = 0; i < kIterations; ++i) {
        const u64 a = random_double(rng);
        for (int mode = 0; mode < 4; ++mode) {
            const fp::Env env = env_for(mode);
            fp::Flags f;
            const u64 ours = fp::round_to_single(a, env, f);

            std::fesetround(kModes[mode]);
            std::feclearexcept(FE_ALL_EXCEPT);
            volatile double x = double_of(a);
            volatile float r = static_cast<float>(x);
            const HostFlags hf = host_flags();
            // Integer conversion oracle: nearbyint in the same mode.
            std::feclearexcept(FE_ALL_EXCEPT);
            volatile double ri = std::nearbyint(static_cast<double>(x));
            std::fesetround(FE_TONEAREST);

            const u64 host = fp::single_to_double_bits(bits_of(static_cast<float>(r)));
            if (!((fp::is_nan(host) && fp::is_nan(ours)) || host == ours) ||
                hf.overflow != f.overflow || hf.underflow != f.underflow ||
                hf.inexact != f.inexact) {
                if (g_reported++ < 15)
                    std::fprintf(stderr, "  frsp mode %d a=%016llX host %016llX ours %016llX\n",
                                 mode, (unsigned long long)a, (unsigned long long)host,
                                 (unsigned long long)ours);
                ++failures;
            }

            bool inv, inx, up;
            const u32 oi = fp::to_int32(a, static_cast<fp::Rounding>(mode), inv, inx, up);
            const double hr = ri;
            const bool host_invalid = std::isnan(hr) || hr > 2147483647.0 || hr < -2147483648.0;
            if (inv != host_invalid ||
                (!inv && (static_cast<s32>(oi) != static_cast<s32>(hr) ||
                          inx != (hr != static_cast<double>(x))))) {
                if (g_reported++ < 15)
                    std::fprintf(stderr, "  to_int32 mode %d a=%016llX host %.17g ours %d inv %d\n",
                                 mode, (unsigned long long)a, hr, static_cast<s32>(oi), inv);
                ++failures;
            }
        }
    }
    CHECK_EQ(failures, 0);
}

TEST_CASE("fpu/ieee754/single_double_bit_conversions")
{
    // Every float exponent, sampled fractions: single -> double -> single
    // must round-trip exactly (lfs then stfs).
    int failures = 0;
    for (u32 e = 0; e < 256; ++e)
        for (u32 frac : {0u, 1u, 0x400000u, 0x7FFFFFu, 0x123456u})
            for (u32 sign : {0u, 0x80000000u}) {
                const u32 s = sign | (e << 23) | frac;
                const u64 d = fp::single_to_double_bits(s);
                if (fp::double_to_single_bits(d) != s)
                    ++failures;
                if (e != 255) {
                    // Value check against the host conversion.
                    if (d != bits_of(static_cast<double>(float_of(s))))
                        ++failures;
                }
            }
    CHECK_EQ(failures, 0);
    // SNaN stays signalling through lfs (PowerPC keeps the bit pattern).
    CHECK_EQ(fp::single_to_double_bits(0x7F800001u), 0x7FF0000020000000ull);
}
