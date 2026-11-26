// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/fpu/ieee754.h — software IEEE 754 binary64 arithmetic with
// binary64 or binary32 result precision.
//
// Why software: the emulated PowerPC FPU needs exact control of rounding and
// complete status information (including "fraction rounded"), identical on
// every host, without depending on the host FPU's flag behaviour (x86 and
// ARM detect underflow differently) or on <cfenv> (unavailable to the
// freestanding core). Correctness first; a hardware fast path can be added
// later where it is proven identical (Milestone 7).
//
// Values are passed as raw binary64 bit patterns (u64). Single-precision
// results are rounded ONCE to binary32 precision and range, then returned
// widened to binary64 format — exactly what the PowerPC stores in an FPR.
//
// NaN operands are NOT handled here (the PowerPC layer applies its own NaN
// propagation rules first). Invalid operations return the default QNaN
// 0x7FF8000000000000 and set the matching flag.
//
// Uses unsigned __int128 (GCC/Clang on 64-bit hosts: x86-64 and AArch64).

#ifndef G3_CORE_FPU_IEEE754_H
#define G3_CORE_FPU_IEEE754_H

#include "core/common/types.h"

namespace g3::fp {

// Values equal the PowerPC FPSCR[RN] encoding.
enum class Rounding : u8 { Nearest = 0, Zero = 1, Up = 2, Down = 3 };
enum class Precision : u8 { Double, Single };
enum class Tininess : u8 { BeforeRounding, AfterRounding };

struct Env {
    Rounding rounding = Rounding::Nearest;
    Tininess tininess = Tininess::BeforeRounding;
    // PowerPC trap-enabled overflow/underflow (FPSCR[OE]/[UE] = 1): instead
    // of overflowing/denormalising, the exponent is adjusted by 1536
    // (double) or 192 (single) and the result delivered (PEM 3.3.6.2).
    bool overflow_adjust = false;
    bool underflow_adjust = false;
};

struct Flags {
    // Invalid-operation causes (PowerPC FPSCR VX* bits).
    bool invalid_isi = false;   // inf - inf
    bool invalid_idi = false;   // inf / inf
    bool invalid_zdz = false;   // 0 / 0
    bool invalid_imz = false;   // inf * 0
    bool invalid_sqrt = false;  // sqrt(negative)
    bool divide_by_zero = false;
    bool overflow = false;
    bool underflow = false;     // reported underflow (depends on env)
    bool inexact = false;
    bool rounded_up = false;    // fraction incremented by rounding (PowerPC FR)

    bool invalid() const
    {
        return invalid_isi || invalid_idi || invalid_zdz || invalid_imz || invalid_sqrt;
    }
};

constexpr u64 kDefaultNaN = 0x7FF8000000000000ull;
constexpr u64 kSignBit = 0x8000000000000000ull;

// Classification of binary64 bit patterns.
constexpr bool is_nan(u64 a) { return (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
constexpr bool is_snan(u64 a) { return is_nan(a) && !(a & 0x0008000000000000ull); }
constexpr bool is_inf(u64 a) { return (a & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
constexpr bool is_zero(u64 a) { return (a & 0x7FFFFFFFFFFFFFFFull) == 0; }
constexpr bool sign_of(u64 a) { return (a >> 63) != 0; }

// Arithmetic. 'p' selects the result precision.
u64 add(u64 a, u64 b, Precision p, const Env& env, Flags& f);
u64 sub(u64 a, u64 b, Precision p, const Env& env, Flags& f);
u64 mul(u64 a, u64 b, Precision p, const Env& env, Flags& f);
u64 div(u64 a, u64 b, Precision p, const Env& env, Flags& f);
// Fused multiply-add: (a * c) + b with a single rounding. negate_addend
// computes (a * c) - b.
u64 fma(u64 a, u64 c, u64 b, bool negate_addend, Precision p, const Env& env, Flags& f);
// Round a binary64 value to binary32 precision/range (PowerPC frsp).
u64 round_to_single(u64 a, const Env& env, Flags& f);
// Square root (used for estimates/tests; the 750 has no fsqrt instruction).
u64 sqrt(u64 a, Precision p, const Env& env, Flags& f);

// Conversion to a 32-bit signed integer (PowerPC fctiw/fctiwz). Sets
// 'invalid' for NaN or out-of-range input; then returns 0x7FFFFFFF or
// 0x80000000 according to the sign (NaN: 0x80000000).
u32 to_int32(u64 a, Rounding r, bool& invalid, bool& inexact, bool& rounded_up);

// Exact format conversions (no rounding).
u64 single_to_double_bits(u32 s);  // PowerPC lfs semantics (keeps SNaN signalling)
// PowerPC stfs semantics (PEM 3.3.6.1 / stfs): no rounding; values in the
// single denormal range are denormalised by truncation.
u32 double_to_single_bits(u64 d);

} // namespace g3::fp

#endif // G3_CORE_FPU_IEEE754_H
