// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/common/bitops.h — bit manipulation helpers, including PowerPC
// "big-endian bit numbering", where bit 0 is the MOST significant bit of a
// 32-bit word and bit 31 the least significant (PowerPC PEM, section 1.2).

#ifndef G3_CORE_COMMON_BITOPS_H
#define G3_CORE_COMMON_BITOPS_H

#include "core/common/types.h"

namespace g3 {

// Rotate left; n is taken modulo 32 (as rlwnm does with the low 5 bits).
constexpr u32 rotl32(u32 v, unsigned n)
{
    n &= 31u;
    return n == 0 ? v : (v << n) | (v >> (32u - n));
}

// Extract the field from PowerPC bit 'first' to PowerPC bit 'last' inclusive
// (0 = MSB). Example: primary opcode = ppc_bits(insn, 0, 5).
constexpr u32 ppc_bits(u32 v, unsigned first, unsigned last)
{
    // Width is last-first+1 (1..32). Shift the field down, then mask it.
    return (v >> (31u - last)) &
           ((last - first + 1u) >= 32u ? 0xFFFFFFFFu : ((1u << (last - first + 1u)) - 1u));
}

// The PowerPC rotate mask MASK(mb, me): ones from bit mb to bit me inclusive
// (PowerPC numbering). If mb > me the mask wraps around, as defined for
// rlwinm/rlwnm/rlwimi in the PEM.
constexpr u32 ppc_mask(unsigned mb, unsigned me)
{
    mb &= 31u;
    me &= 31u;
    const u32 from_mb = 0xFFFFFFFFu >> mb;          // ones from bit mb to 31
    const u32 to_me   = 0xFFFFFFFFu << (31u - me);  // ones from bit 0 to me
    return mb <= me ? (from_mb & to_me) : (from_mb | to_me);
}

constexpr s32 sign_extend16(u32 v) { return static_cast<s32>(static_cast<s16>(static_cast<u16>(v))); }
constexpr s32 sign_extend8(u32 v)  { return static_cast<s32>(static_cast<s8>(static_cast<u8>(v))); }

// Sign-extend the low 'bits' bits of v (1..32).
constexpr s32 sign_extend(u32 v, unsigned bits)
{
    const unsigned shift = 32u - bits;
    return static_cast<s32>(v << shift) >> shift;
}

// Count leading zeros with a defined result for 0 (cntlzw returns 32).
constexpr u32 count_leading_zeros32(u32 v)
{
    return v == 0 ? 32u : static_cast<u32>(__builtin_clz(v));
}

} // namespace g3

#endif // G3_CORE_COMMON_BITOPS_H
