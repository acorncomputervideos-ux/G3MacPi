// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the 750 FPU instructions NOT covered by the hardware
// vectors: FP loads/stores, conversions, moves, fsel, FPSCR instructions,
// estimates, NaN propagation and FP exceptions. Expected values come from
// the PEM and, for bit patterns, were computed independently (Python struct).

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <initializer_list>

using namespace g3;
using namespace g3::ppc;
using namespace asm_ppc;

namespace {

constexpr u32 kCode = 0x1000;
constexpr u32 kData = 0x8000;
constexpr u64 kOne = 0x3FF0000000000000ull;
constexpr u64 kTwo = 0x4000000000000000ull;
constexpr u64 kThree = 0x4008000000000000ull;
constexpr u64 kOnePointFive = 0x3FF8000000000000ull;
constexpr u64 kMinusOnePointFive = 0xBFF8000000000000ull;
constexpr u64 kPointOne = 0x3FB999999999999Aull;
constexpr u64 kInf = 0x7FF0000000000000ull;
constexpr u64 kNegZero = 0x8000000000000000ull;

struct FpMachine {
    u8 ram[64 * 1024] = {};
    FlatMemory mem{ram, sizeof ram};
    Ppc750 cpu{mem};
    FpMachine()
    {
        cpu.state().pc = kCode;
        cpu.state().msr = msr::FP;
    }
    // Executes the given instructions one after another.
    void exec(std::initializer_list<u32> insns)
    {
        u32 a = cpu.state().pc;
        for (u32 w : insns) {
            mem.write32(a, w);
            a += 4;
        }
        for (size_t i = 0; i < insns.size(); ++i)
            cpu.step();
    }
    CpuState& s() { return cpu.state(); }
    u32 word(u32 a)
    {
        u32 v = 0;
        mem.read32(a, v);
        return v;
    }
};

} // namespace

// --- loads and stores -------------------------------------------------------------

TEST_CASE("cpu/fpu/lfs_stfs_conversions")
{
    FpMachine m;
    m.s().gpr[3] = kData;
    m.mem.write32(kData, 0x3FC00000u);  // 1.5f
    m.s().fpr[2] = kPointOne;
    m.s().fpr[4] = 0x37D0000000000000ull;  // 2^-130: single denormal range
    m.exec({lfs(1, 0, 3), stfs(2, 4, 3), stfs(4, 8, 3), stfs(1, 12, 3)});
    CHECK_EQ(m.s().fpr[1], kOnePointFive);
    CHECK_EQ(m.word(kData + 4), 0x3DCCCCCCu);  // stfs truncates (no rounding)
    CHECK_EQ(m.word(kData + 8), 0x00080000u);  // denormalised
    CHECK_EQ(m.word(kData + 12), 0x3FC00000u);
    CHECK_EQ(m.s().fpscr, 0u);                 // loads/stores do not touch FPSCR
}

TEST_CASE("cpu/fpu/lfd_stfd_update_indexed_stfiwx")
{
    FpMachine m;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 16;
    m.s().fpr[5] = 0x0123456789ABCDEFull;
    m.exec({stfdu(5, 8, 3), lfdx(6, 3, 4), lfd(7, 0, 3), stfiwx(5, 0, 4)});
    CHECK_EQ(m.s().gpr[3], kData + 8u);
    CHECK_EQ(m.word(kData + 8), 0x01234567u);
    CHECK_EQ(m.word(kData + 12), 0x89ABCDEFu);
    CHECK_EQ(m.s().fpr[7], 0x0123456789ABCDEFull);
    CHECK_EQ(m.s().fpr[6], 0u);           // kData+24 is zero
    CHECK_EQ(m.word(16), 0x89ABCDEFu);    // stfiwx: low word, EA = 0 + r4
}

TEST_CASE("cpu/fpu/lfsu_updates_ra")
{
    FpMachine m;
    m.s().gpr[3] = kData;
    m.mem.write32(kData + 4, 0x3F800000u);  // 1.0f
    m.exec({lfsu(1, 4, 3)});
    CHECK_EQ(m.s().fpr[1], kOne);
    CHECK_EQ(m.s().gpr[3], kData + 4u);
}

// --- compares ---------------------------------------------------------------------

TEST_CASE("cpu/fpu/fcmpu_and_fcmpo")
{
    FpMachine m;
    m.s().fpr[1] = kOne;
    m.s().fpr[2] = kTwo;
    m.s().fpr[3] = 0;
    m.s().fpr[4] = kNegZero;
    m.s().fpr[5] = 0x7FF8000000000000ull;  // QNaN
    m.exec({fcmpu(0, 1, 2), fcmpu(1, 2, 1), fcmpu(2, 3, 4), fcmpu(3, 1, 5)});
    CHECK_EQ(m.s().cr >> 28, crf::LT);
    CHECK_EQ((m.s().cr >> 24) & 0xF, crf::GT);
    CHECK_EQ((m.s().cr >> 20) & 0xF, crf::EQ);       // -0 == +0
    CHECK_EQ((m.s().cr >> 16) & 0xF, 1u);            // unordered
    CHECK_EQ(m.s().fpscr & fpscr::FPCC, 1u << 12);   // FU
    CHECK_EQ(m.s().fpscr & fpscr::VXVC, 0u);         // fcmpu: QNaN is quiet
    m.exec({fcmpo(4, 1, 5)});
    CHECK_EQ(m.s().fpscr & (fpscr::VXVC | fpscr::VX | fpscr::FX),
             fpscr::VXVC | fpscr::VX | fpscr::FX);
}

// --- conversions -------------------------------------------------------------------

TEST_CASE("cpu/fpu/fctiw_fctiwz")
{
    FpMachine m;
    m.s().fpr[1] = 0x4004000000000000ull;  // 2.5
    m.s().fpr[2] = 0xC005984A0E410B63ull;  // about -2.7
    m.s().fpr[3] = 0x41E65A0BC0000000ull;  // 3e9 (> INT32_MAX)
    m.s().fpr[4] = 0x7FF8000000000000ull;  // NaN
    m.exec({fctiw(10, 1), fctiwz(11, 2)});
    CHECK_EQ(m.s().fpr[10] & 0xFFFFFFFFu, 2u);           // ties to even
    CHECK_EQ(m.s().fpr[11] & 0xFFFFFFFFu, 0xFFFFFFFEu);  // -2, toward zero
    CHECK(m.s().fpscr & fpscr::XX);
    CHECK_EQ(m.s().fpscr & fpscr::VX, 0u);
    m.exec({fctiw(12, 3), fctiw(13, 4)});
    CHECK_EQ(m.s().fpr[12] & 0xFFFFFFFFu, 0x7FFFFFFFu);
    CHECK_EQ(m.s().fpr[13] & 0xFFFFFFFFu, 0x80000000u);
    CHECK(m.s().fpscr & fpscr::VXCVI);
    CHECK(m.s().fpscr & fpscr::VX);
}

TEST_CASE("cpu/fpu/frsp")
{
    FpMachine m;
    m.s().fpr[1] = kPointOne;
    m.exec({frsp(2, 1)});
    CHECK_EQ(m.s().fpr[2], 0x3FB99999A0000000ull);  // rounded (up) to single
    CHECK(m.s().fpscr & fpscr::FR);
    CHECK(m.s().fpscr & fpscr::FI);
    CHECK(m.s().fpscr & fpscr::XX);
    CHECK_EQ(m.s().fpscr & fpscr::FPRF, fpscr::kClassPosNorm);
}

// --- moves and fsel ----------------------------------------------------------------

TEST_CASE("cpu/fpu/moves_do_not_touch_fpscr")
{
    FpMachine m;
    m.s().fpr[1] = kMinusOnePointFive;
    m.s().fpscr = 0x00000003u;
    m.exec({fneg(2, 1), fabs_(3, 1), fnabs(4, 2), fmr(5, 1)});
    CHECK_EQ(m.s().fpr[2], kOnePointFive);
    CHECK_EQ(m.s().fpr[3], kOnePointFive);
    CHECK_EQ(m.s().fpr[4], kMinusOnePointFive);
    CHECK_EQ(m.s().fpr[5], kMinusOnePointFive);
    CHECK_EQ(m.s().fpscr, 0x00000003u);
}

TEST_CASE("cpu/fpu/fsel")
{
    FpMachine m;
    m.s().fpr[2] = kOne;    // frC
    m.s().fpr[3] = kTwo;    // frB
    m.s().fpr[10] = kNegZero;
    m.s().fpr[11] = kMinusOnePointFive;
    m.s().fpr[12] = 0x7FF8000000000000ull;
    m.exec({fsel(20, 10, 2, 3), fsel(21, 11, 2, 3), fsel(22, 12, 2, 3)});
    CHECK_EQ(m.s().fpr[20], kOne);  // -0 >= 0
    CHECK_EQ(m.s().fpr[21], kTwo);
    CHECK_EQ(m.s().fpr[22], kTwo);  // NaN selects frB
}

// --- arithmetic details not in the vectors -------------------------------------

TEST_CASE("cpu/fpu/fnmadd_negates_result")
{
    FpMachine m;
    m.s().fpr[1] = kOne;
    m.s().fpr[2] = kTwo;
    m.s().fpr[3] = kThree;
    m.exec({fnmadd(4, 1, 2, 3)});  // -(1*2 + 3)
    CHECK_EQ(m.s().fpr[4], 0xC014000000000000ull);
    CHECK_EQ(m.s().fpscr & fpscr::FPRF, fpscr::kClassNegNorm);
}

TEST_CASE("cpu/fpu/nan_propagation")
{
    FpMachine m;
    m.s().fpr[1] = 0x7FF8000000000001ull;  // QNaN payload 1
    m.s().fpr[2] = 0x7FF8000000000002ull;  // QNaN payload 2
    m.s().fpr[3] = 0x7FF0000000000001ull;  // SNaN
    m.exec({fadd(4, 1, 2), fadd(5, 2, 3), fadds(6, 1, 2)});
    CHECK_EQ(m.s().fpr[4], 0x7FF8000000000001ull);   // frA wins
    CHECK_EQ(m.s().fpr[5], 0x7FF8000000000002ull);   // frA (QNaN) wins over SNaN frB
    CHECK(m.s().fpscr & fpscr::VXSNAN);              // ... but the SNaN is reported
    CHECK_EQ(m.s().fpr[6], 0x7FF8000000000000ull);   // single: fraction truncated
    CHECK_EQ(m.s().fpscr & fpscr::FPRF, fpscr::kClassQNaN);
}

TEST_CASE("cpu/fpu/record_form_sets_cr1")
{
    FpMachine m;
    m.s().fpr[1] = kOne;
    m.s().fpr[2] = 0;
    m.exec({fdiv(3, 1, 2, 1)});  // 1/0
    CHECK_EQ(m.s().fpr[3], kInf);
    CHECK(m.s().fpscr & fpscr::ZX);
    CHECK_EQ((m.s().cr >> 24) & 0xF, 8u);  // CR1 = FX FEX VX OX = 1000
}

TEST_CASE("cpu/fpu/estimates")
{
    FpMachine m;
    m.s().fpr[1] = kTwo;
    m.s().fpr[2] = 0x4010000000000000ull;  // 4.0
    m.s().fpr[3] = 0xBFF0000000000000ull;  // -1.0
    m.s().fpr[4] = 0;
    m.exec({fres(10, 1), frsqrte(11, 2)});
    CHECK_EQ(m.s().fpr[10], 0x3FE0000000000000ull);  // 0.5
    CHECK_EQ(m.s().fpr[11], 0x3FE0000000000000ull);  // 1/sqrt(4)
    m.exec({frsqrte(12, 3)});
    CHECK(m.s().fpscr & fpscr::VXSQRT);
    m.exec({fres(13, 4)});
    CHECK_EQ(m.s().fpr[13], kInf);
    CHECK(m.s().fpscr & fpscr::ZX);
}

TEST_CASE("cpu/fpu/fsqrt_is_illegal_on_750")
{
    FpMachine m;
    m.exec({fsqrt(1, 2)});
    CHECK_EQ(m.s().pc, 0x700u);
    CHECK_EQ(m.s().spr[spr::SRR1] & srr1::ILLEGAL, srr1::ILLEGAL);
}

// --- FPSCR instructions ------------------------------------------------------------

TEST_CASE("cpu/fpu/mtfsf_mffs")
{
    FpMachine m;
    m.s().fpr[1] = 0x00000003u;
    m.exec({mtfsf(0x01, 1), mffs(2)});
    CHECK_EQ(m.s().fpscr, 3u);  // only field 7 (RN) written
    CHECK_EQ(m.s().fpr[2] & 0xFFFFFFFFu, 3u);
    // FEX and VX cannot be written; they are recomputed from the other bits.
    m.s().fpr[3] = 0x60000000u;  // FEX | VX only
    m.exec({mtfsf(0xFF, 3)});
    CHECK_EQ(m.s().fpscr, 0u);
    m.s().fpr[4] = fpscr::VXISI | fpscr::VE;
    m.exec({mtfsf(0xFF, 4)});
    CHECK_EQ(m.s().fpscr & (fpscr::VX | fpscr::FEX), fpscr::VX | fpscr::FEX);
}

TEST_CASE("cpu/fpu/mtfsfi_mtfsb")
{
    FpMachine m;
    m.exec({mtfsfi(7, 1)});
    CHECK_EQ(m.s().fpscr, 1u);
    m.exec({mtfsb1(30), mtfsb0(31)});
    CHECK_EQ(m.s().fpscr, 2u);
    m.exec({mtfsb1(3)});  // OX: an exception bit -> FX too
    CHECK_EQ(m.s().fpscr & (fpscr::OX | fpscr::FX), fpscr::OX | fpscr::FX);
    m.exec({mtfsb1(1)});  // FEX cannot be set explicitly
    CHECK_EQ(m.s().fpscr & fpscr::FEX, 0u);
}

TEST_CASE("cpu/fpu/mcrfs_copies_and_clears")
{
    FpMachine m;
    m.s().fpscr = fpscr::FX | fpscr::OX | 0x2u;
    m.exec({mcrfs(2, 0), mcrfs(3, 7)});
    CHECK_EQ((m.s().cr >> 20) & 0xF, 9u);  // FX . . OX
    CHECK_EQ((m.s().cr >> 16) & 0xF, 2u);  // RN field
    CHECK_EQ(m.s().fpscr, 2u);             // FX and OX cleared, RN kept
}

// --- exceptions ---------------------------------------------------------------------

TEST_CASE("cpu/fpu/fp_enabled_program_exception")
{
    FpMachine m;
    m.s().msr = msr::FP | msr::FE0 | msr::FE1;
    m.s().fpscr = fpscr::VE;
    m.s().fpr[1] = kInf;
    m.s().fpr[2] = kInf | fp::kSignBit;
    m.s().fpr[3] = kOne;
    m.exec({fadd(3, 1, 2)});  // inf + -inf: invalid
    CHECK_EQ(m.s().pc, 0x700u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode);
    CHECK_EQ(m.s().spr[spr::SRR1] & srr1::FP_ENABLED, srr1::FP_ENABLED);
    CHECK_EQ(m.s().fpr[3], kOne);  // result suppressed
    CHECK(m.s().fpscr & fpscr::VXISI);
}

TEST_CASE("cpu/fpu/no_exception_when_fe_bits_clear")
{
    FpMachine m;
    m.s().fpscr = fpscr::VE;
    m.s().fpr[1] = kInf;
    m.s().fpr[2] = kInf | fp::kSignBit;
    m.exec({fadd(3, 1, 2)});
    CHECK_EQ(m.s().pc, kCode + 4u);  // FE0 = FE1 = 0: ignored
    CHECK(m.s().fpscr & fpscr::FEX);
}
