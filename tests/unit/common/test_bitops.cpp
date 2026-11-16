// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/common/bitops.h
//
// Expected values for ppc_mask are derived by hand from the MASK(mb, me)
// definition in the PowerPC Programming Environments Manual (rlwinm).

#include "core/common/bitops.h"
#include "tests/framework/g3test.h"

using namespace g3;

TEST_CASE("common/bitops/rotl32")
{
    CHECK_EQ(rotl32(0x80000001u, 1), 0x00000003u);
    CHECK_EQ(rotl32(0x12345678u, 0), 0x12345678u);
    CHECK_EQ(rotl32(0x12345678u, 32), 0x12345678u);  // modulo 32
    CHECK_EQ(rotl32(0x12345678u, 8), 0x34567812u);
    CHECK_EQ(rotl32(0x12345678u, 31), 0x091A2B3Cu);
}

TEST_CASE("common/bitops/ppc_bits")
{
    // addi r3, r1, 8  = 0x38610008: opcode 14, rD 3, rA 1, SIMM 8
    const u32 insn = 0x38610008u;
    CHECK_EQ(ppc_bits(insn, 0, 5), 14u);
    CHECK_EQ(ppc_bits(insn, 6, 10), 3u);
    CHECK_EQ(ppc_bits(insn, 11, 15), 1u);
    CHECK_EQ(ppc_bits(insn, 16, 31), 8u);
    CHECK_EQ(ppc_bits(0xFFFFFFFFu, 0, 31), 0xFFFFFFFFu);
    CHECK_EQ(ppc_bits(0x80000000u, 0, 0), 1u);
    CHECK_EQ(ppc_bits(0x00000001u, 31, 31), 1u);
}

TEST_CASE("common/bitops/ppc_mask")
{
    CHECK_EQ(ppc_mask(0, 31), 0xFFFFFFFFu);
    CHECK_EQ(ppc_mask(0, 0), 0x80000000u);
    CHECK_EQ(ppc_mask(31, 31), 0x00000001u);
    CHECK_EQ(ppc_mask(16, 31), 0x0000FFFFu);
    CHECK_EQ(ppc_mask(0, 15), 0xFFFF0000u);
    CHECK_EQ(ppc_mask(24, 27), 0x000000F0u);
    // Wrap-around masks (mb > me).
    CHECK_EQ(ppc_mask(31, 0), 0x80000001u);
    CHECK_EQ(ppc_mask(28, 3), 0xF000000Fu);
    CHECK_EQ(ppc_mask(1, 0), 0xFFFFFFFFu);
}

TEST_CASE("common/bitops/sign_extend")
{
    CHECK_EQ(sign_extend16(0x7FFFu), 32767);
    CHECK_EQ(sign_extend16(0x8000u), -32768);
    CHECK_EQ(sign_extend16(0x1FFFFu), -1);  // upper bits ignored
    CHECK_EQ(sign_extend8(0x80u), -128);
    CHECK_EQ(sign_extend8(0x7Fu), 127);
    CHECK_EQ(sign_extend(0x3FFFFFCu, 26), -4);  // 26-bit branch displacement
    CHECK_EQ(sign_extend(0x1FFFFFCu, 26), 0x1FFFFFC);
    CHECK_EQ(sign_extend(0xFFFFFFFFu, 32), -1);
}

TEST_CASE("common/bitops/count_leading_zeros32")
{
    CHECK_EQ(count_leading_zeros32(0), 32u);
    CHECK_EQ(count_leading_zeros32(1), 31u);
    CHECK_EQ(count_leading_zeros32(0x80000000u), 0u);
    CHECK_EQ(count_leading_zeros32(0x00010000u), 15u);
}

// Compile-time checks: these helpers must be usable in constant expressions.
static_assert(ppc_mask(16, 31) == 0x0000FFFFu, "ppc_mask constexpr");
static_assert(rotl32(1u, 4) == 16u, "rotl32 constexpr");
static_assert(ppc_bits(0x38610008u, 0, 5) == 14u, "ppc_bits constexpr");
