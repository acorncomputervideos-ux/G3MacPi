// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the PowerPC 750 interpreter: branches, condition register,
// loads/stores, SPRs, exceptions and interrupts. Expected values are derived
// by hand from the PowerPC Programming Environments Manual (PEM).

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <initializer_list>

using namespace g3;
using namespace g3::ppc;
using namespace asm_ppc;

namespace {

constexpr u32 kRamSize = 64 * 1024;
constexpr u32 kCode = 0x1000;
constexpr u32 kData = 0x8000;

struct Machine {
    u8 ram[kRamSize] = {};
    FlatMemory mem{ram, kRamSize};
    Ppc750 cpu{mem};

    Machine()
    {
        cpu.state().pc = kCode;
        cpu.state().msr = 0;  // supervisor, real mode, vectors at 0x00000000
    }
    void load(u32 addr, std::initializer_list<u32> words)
    {
        for (u32 w : words) {
            mem.write32(addr, w);
            addr += 4;
        }
    }
    // Runs until pc == stop (returns true) or max_steps elapsed (false).
    bool run_to(u32 stop, int max_steps = 10000)
    {
        for (int i = 0; i < max_steps; ++i) {
            if (cpu.state().pc == stop)
                return true;
            cpu.step();
        }
        return cpu.state().pc == stop;
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

// --- reset -----------------------------------------------------------------------

TEST_CASE("cpu/ppc750/reset_state")
{
    u8 ram[16];
    FlatMemory mem(ram, sizeof ram);
    Ppc750 cpu(mem, 0x00081234u);
    CHECK_EQ(cpu.state().pc, 0xFFF00100u);
    CHECK_EQ(cpu.state().msr, msr::IP);
    CHECK_EQ(cpu.state().spr[spr::PVR], 0x00081234u);
    CHECK_EQ(cpu.state().gpr[1], 0u);
    CHECK(!cpu.checkstopped());
}

// --- branches -----------------------------------------------------------------------

TEST_CASE("cpu/ppc750/branch_loop_bdnz")
{
    // r3 = sum 1..10 using CTR: li r3,0; li r4,10; mtctr r4; loop: add r3,r3,r4;
    // addi r4,r4,-1; bdnz loop
    Machine m;
    m.load(kCode, {li(3, 0), li(4, 10), mtspr(spr::CTR, 4),
                   add(3, 3, 4), addi(4, 4, 0xFFFF), bdnz(-8)});
    REQUIRE(m.run_to(kCode + 24));
    CHECK_EQ(m.s().gpr[3], 55u);
    CHECK_EQ(m.s().ctr(), 0u);
}

TEST_CASE("cpu/ppc750/branch_link_and_return")
{
    Machine m;
    // 0x1000: bl +12 -> 0x100C ; 0x1004: li r5,2 ; 0x1008: b +0x100 (end)
    // 0x100C: li r5,1 ; 0x1010: blr
    m.load(kCode, {b(12, false, true), li(5, 2), b(0x100), li(5, 1), blr()});
    REQUIRE(m.run_to(kCode + 0x108));
    CHECK_EQ(m.s().lr(), kCode + 4u);
    CHECK_EQ(m.s().gpr[5], 2u);  // returned and continued at 0x1004
}

TEST_CASE("cpu/ppc750/branch_absolute_and_ctr")
{
    Machine m;
    m.load(kCode, {b(0x2000, true)});                                 // ba 0x2000
    m.load(0x2000, {li(6, 0x3000), mtspr(spr::CTR, 6), bctr()});
    m.load(0x3000, {li(7, 7)});
    REQUIRE(m.run_to(0x3004));
    CHECK_EQ(m.s().gpr[7], 7u);
}

TEST_CASE("cpu/ppc750/branch_conditional_on_compare")
{
    Machine m;
    // li r3,5; cmpwi r3,5; beq +8; li r4,1 (skipped); li r5,9; cmpwi cr1,r3,6; bne cr1,+8; li r6,1 (skipped)
    m.load(kCode, {li(3, 5), cmpwi(0, 3, 5), beq(8), li(4, 1), li(5, 9),
                   cmpwi(1, 3, 6), bne(8, 1), li(6, 1)});
    REQUIRE(m.run_to(kCode + 32));
    CHECK_EQ(m.s().gpr[4], 0u);
    CHECK_EQ(m.s().gpr[5], 9u);
    CHECK_EQ(m.s().gpr[6], 0u);
    CHECK_EQ(m.s().cr >> 28, crf::EQ);         // cr0: 5 == 5
    CHECK_EQ((m.s().cr >> 24) & 0xF, crf::LT);  // cr1: 5 < 6
}

TEST_CASE("cpu/ppc750/blrl_uses_old_lr")
{
    Machine m;
    m.s().lr() = 0x2000;
    m.load(kCode, {blrl()});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x2000u);
    CHECK_EQ(m.s().lr(), kCode + 4u);
}

TEST_CASE("cpu/ppc750/bc_not_taken_falls_through_and_decrements")
{
    Machine m;
    m.s().ctr() = 1;
    m.load(kCode, {bdnz(0x100)});  // CTR -> 0, so not taken
    m.cpu.step();
    CHECK_EQ(m.s().pc, kCode + 4u);
    CHECK_EQ(m.s().ctr(), 0u);
}

// --- condition register ---------------------------------------------------------

TEST_CASE("cpu/ppc750/cr_logical_ops")
{
    // CR bits 0 = 1, 1 = 0. Each op writes its result into bit 8+n.
    const u32 ops[8] = {257, 129, 289, 225, 33, 449, 417, 193};
    // and andc eqv nand nor or orc xor, with a=1 b=0:
    const bool expected[8] = {false, true, false, true, false, true, true, true};
    for (int i = 0; i < 8; ++i) {
        Machine m;
        m.s().cr = 0x80000000u;
        m.load(kCode, {crop(ops[i], 8, 0, 1)});
        m.cpu.step();
        CHECK_EQ(((m.s().cr >> (31 - 8)) & 1u) != 0, expected[i]);
    }
}

TEST_CASE("cpu/ppc750/mtcrf_mfcr_mcrf_mcrxr")
{
    Machine m;
    m.s().gpr[3] = 0x12345678u;
    m.s().xer() = 0xA0000000u;  // SO and CA
    m.load(kCode, {mtcrf(0x81, 3), mfcr(4), mcrf(2, 0), mcrxr(3)});
    m.run_to(kCode + 16);
    CHECK_EQ(m.s().gpr[4], 0x10000008u);              // only fields 0 and 7 copied
    CHECK_EQ((m.s().cr >> 20) & 0xF, 1u);             // cr2 = cr0 = 0x1
    CHECK_EQ((m.s().cr >> 16) & 0xF, 0xAu);           // cr3 = XER[0-3]
    CHECK_EQ(m.s().xer(), 0u);                         // XER[0-3] cleared
}

// --- rotate (rlwnm is not covered by the vector file) --------------------------

TEST_CASE("cpu/ppc750/rlwnm")
{
    Machine m;
    m.s().gpr[3] = 0x12345678u;
    m.s().gpr[4] = 36;  // only the low 5 bits count: rotate by 4
    m.load(kCode, {rlwnm(5, 3, 4, 0, 31), rlwnm(6, 3, 4, 24, 31, 1)});
    m.run_to(kCode + 8);
    CHECK_EQ(m.s().gpr[5], 0x23456781u);
    CHECK_EQ(m.s().gpr[6], 0x00000081u);
    CHECK_EQ(m.s().cr >> 28, crf::GT);
}

// --- loads and stores -------------------------------------------------------------

TEST_CASE("cpu/ppc750/store_load_big_endian")
{
    Machine m;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 0x11223344u;
    m.load(kCode, {stw(4, 0, 3), lbz(5, 0, 3), lbz(6, 3, 3), lhz(7, 2, 3), lwz(8, 0, 3)});
    m.run_to(kCode + 20);
    CHECK_EQ(m.ram[kData], 0x11u);  // big-endian in guest memory
    CHECK_EQ(m.ram[kData + 3], 0x44u);
    CHECK_EQ(m.s().gpr[5], 0x11u);
    CHECK_EQ(m.s().gpr[6], 0x44u);
    CHECK_EQ(m.s().gpr[7], 0x3344u);
    CHECK_EQ(m.s().gpr[8], 0x11223344u);
}

TEST_CASE("cpu/ppc750/lha_sign_extends_and_sth_stb")
{
    Machine m;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 0xABCD8001u;
    m.load(kCode, {sth(4, 0, 3), stb(4, 2, 3), lha(5, 0, 3), lhz(6, 0, 3), lbz(7, 2, 3)});
    m.run_to(kCode + 20);
    CHECK_EQ(m.s().gpr[5], 0xFFFF8001u);
    CHECK_EQ(m.s().gpr[6], 0x00008001u);
    CHECK_EQ(m.s().gpr[7], 0x01u);
}

TEST_CASE("cpu/ppc750/update_forms")
{
    Machine m;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 0xCAFEF00Du;
    m.s().gpr[9] = 4;
    m.load(kCode, {stwu(4, 8, 3), lwzu(5, 0, 3), lbzux(6, 3, 9)});
    m.run_to(kCode + 12);
    CHECK_EQ(m.word(kData + 8), 0xCAFEF00Du);
    CHECK_EQ(m.s().gpr[5], 0xCAFEF00Du);
    CHECK_EQ(m.s().gpr[3], kData + 12u);   // 8, then +0, then +4
    CHECK_EQ(m.s().gpr[6], 0u);            // byte at kData+12
}

TEST_CASE("cpu/ppc750/byte_reversed")
{
    Machine m;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 0x11223344u;
    m.load(kCode, {stwbrx(4, 0, 3), lwz(5, 0, 3), lwbrx(6, 0, 3), lhbrx(7, 0, 3)});
    m.run_to(kCode + 16);
    CHECK_EQ(m.s().gpr[5], 0x44332211u);
    CHECK_EQ(m.s().gpr[6], 0x11223344u);
    CHECK_EQ(m.s().gpr[7], 0x3344u);
}

TEST_CASE("cpu/ppc750/lmw_stmw")
{
    Machine m;
    m.s().gpr[3] = kData;
    for (u32 r = 28; r < 32; ++r)
        m.s().gpr[r] = 0x100u + r;
    m.load(kCode, {stmw(28, 0x10, 3)});
    m.cpu.step();
    CHECK_EQ(m.word(kData + 0x10), 0x11Cu);
    CHECK_EQ(m.word(kData + 0x1C), 0x11Fu);
    for (u32 r = 28; r < 32; ++r)
        m.s().gpr[r] = 0;
    m.load(kCode + 4, {lmw(29, 0x14, 3)});
    m.cpu.step();
    CHECK_EQ(m.s().gpr[28], 0u);   // not loaded
    CHECK_EQ(m.s().gpr[29], 0x11Du);
    CHECK_EQ(m.s().gpr[31], 0x11Fu);
}

TEST_CASE("cpu/ppc750/string_instructions")
{
    Machine m;
    m.s().gpr[3] = kData;
    const u8 text[7] = {'G', '3', 'M', 'a', 'c', 'P', 'i'};
    for (int i = 0; i < 7; ++i)
        m.ram[kData + i] = text[i];
    m.s().gpr[6] = 0xFFFFFFFFu;
    m.load(kCode, {lswi(5, 3, 7)});
    m.cpu.step();
    CHECK_EQ(m.s().gpr[5], 0x47334D61u);  // "G3Ma"
    CHECK_EQ(m.s().gpr[6], 0x63506900u);  // "cPi" + zero fill
    m.load(kCode + 4, {stswi(5, 3, 5)});
    m.ram[kData + 4] = 0;
    m.ram[kData + 5] = 0x77;
    m.cpu.step();
    CHECK_EQ(m.ram[kData + 4], 'c');
    CHECK_EQ(m.ram[kData + 5], 0x77u);  // only 5 bytes stored
}

TEST_CASE("cpu/ppc750/lwarx_stwcx")
{
    Machine m;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 0;
    m.s().gpr[5] = 42;
    m.load(kCode, {lwarx(6, 3, 4), stwcx(5, 3, 4), stwcx(5, 3, 4)});
    m.cpu.step();
    m.cpu.step();
    CHECK_EQ(m.word(kData), 42u);
    CHECK_EQ(m.s().cr >> 28, crf::EQ);  // first stwcx. succeeded
    m.mem.write32(kData, 0);
    m.cpu.step();
    CHECK_EQ(m.word(kData), 0u);        // reservation gone: not stored
    CHECK_EQ(m.s().cr >> 28, 0u);
}

TEST_CASE("cpu/ppc750/lwarx_misaligned_alignment_exception")
{
    Machine m;
    m.s().gpr[3] = kData + 2;
    m.s().gpr[4] = 0;
    m.load(kCode, {lwarx(6, 3, 4)});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x600u);
    CHECK_EQ(m.s().spr[spr::DAR], kData + 2u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode);
}

TEST_CASE("cpu/ppc750/dcbz_zeroes_32_byte_block")
{
    Machine m;
    for (u32 i = 0; i < 64; ++i)
        m.ram[kData + i] = 0xEE;
    m.s().gpr[3] = kData;
    m.s().gpr[4] = 37;  // inside the second block
    m.load(kCode, {dcbz(3, 4)});
    m.cpu.step();
    CHECK_EQ(m.ram[kData + 31], 0xEEu);
    CHECK_EQ(m.ram[kData + 32], 0u);
    CHECK_EQ(m.ram[kData + 63], 0u);
}

// --- SPRs -------------------------------------------------------------------------

TEST_CASE("cpu/ppc750/spr_access")
{
    Machine m;
    m.s().gpr[3] = 0xFFFFFFFFu;
    m.load(kCode, {mtspr(spr::XER, 3), mfspr(4, spr::XER), mtspr(spr::SPRG2, 3),
                   mfspr(5, spr::SPRG2), mfspr(6, spr::PVR), mtsr(5, 3), mfsr(7, 5)});
    REQUIRE(m.run_to(kCode + 28));
    CHECK_EQ(m.s().gpr[4], 0xE000007Fu);  // only implemented XER bits
    CHECK_EQ(m.s().gpr[5], 0xFFFFFFFFu);
    CHECK_EQ(m.s().gpr[6], kPvrMpc750Placeholder);
    CHECK_EQ(m.s().gpr[7], 0xFFFFFFFFu);
}

TEST_CASE("cpu/ppc750/timebase_and_mftb")
{
    Machine m;
    m.cpu.advance_timebase(0x12345678u);
    m.cpu.advance_timebase(0xF0000000u);  // carries into TBU
    m.load(kCode, {mftb(3, spr::TBL_READ), mftb(4, spr::TBU_READ)});
    m.run_to(kCode + 8);
    CHECK_EQ(m.s().gpr[3], 0x02345678u);
    CHECK_EQ(m.s().gpr[4], 1u);
}

// --- exceptions -----------------------------------------------------------------

TEST_CASE("cpu/ppc750/system_call_and_rfi")
{
    Machine m;
    m.s().msr = msr::EE | msr::ME | msr::PR | msr::FP;
    m.load(kCode, {sc()});
    m.load(0xC00, {rfi()});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0xC00u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode + 4u);
    CHECK_EQ(m.s().spr[spr::SRR1], msr::EE | msr::ME | msr::PR | msr::FP);
    CHECK_EQ(m.s().msr, msr::ME);  // everything else cleared, ME kept
    m.cpu.step();                  // rfi
    CHECK_EQ(m.s().pc, kCode + 4u);
    CHECK_EQ(m.s().msr, msr::EE | msr::ME | msr::PR | msr::FP);
}

TEST_CASE("cpu/ppc750/exception_prefix_ip")
{
    Machine m;
    m.s().msr = msr::IP;
    m.load(kCode, {sc()});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0xFFF00C00u);
    CHECK_EQ(m.s().msr, msr::IP);
}

TEST_CASE("cpu/ppc750/illegal_instruction")
{
    Machine m;
    m.load(kCode, {0x00000000u});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x700u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode);
    CHECK_EQ(m.s().spr[spr::SRR1] & srr1::ILLEGAL, srr1::ILLEGAL);
}

TEST_CASE("cpu/ppc750/privileged_in_user_mode")
{
    const u32 insns[] = {mfmsr(3), mtmsr(3), rfi(), mfspr(3, spr::SPRG0), mtspr(spr::SRR0, 3),
                         mtsr(1, 3)};
    for (u32 insn : insns) {
        Machine m;
        m.s().msr = msr::PR;
        m.load(kCode, {insn});
        m.cpu.step();
        CHECK_EQ(m.s().pc, 0x700u);
        CHECK_EQ(m.s().spr[spr::SRR1] & srr1::PRIVILEGED, srr1::PRIVILEGED);
        CHECK_EQ(m.s().spr[spr::SRR1] & msr::PR, msr::PR);
    }
}

TEST_CASE("cpu/ppc750/unimplemented_spr_is_illegal")
{
    Machine m;
    m.load(kCode, {mfspr(3, 1000)});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x700u);
    CHECK_EQ(m.s().spr[spr::SRR1] & srr1::ILLEGAL, srr1::ILLEGAL);
}

TEST_CASE("cpu/ppc750/traps")
{
    Machine m;
    m.s().gpr[3] = 5;
    m.s().gpr[4] = 7;
    m.load(kCode, {tw(0x04, 3, 4)});  // trap if equal: no trap
    m.cpu.step();
    CHECK_EQ(m.s().pc, kCode + 4u);
    m.load(kCode + 4, {twi(0x10, 3, 6)});  // trap if 5 < 6 (signed)
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x700u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode + 4u);
    CHECK_EQ(m.s().spr[spr::SRR1] & srr1::TRAP, srr1::TRAP);
}

TEST_CASE("cpu/ppc750/fp_unavailable")
{
    Machine m;
    m.load(kCode, {lfd(1, 0, 3)});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x800u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode);
}

TEST_CASE("cpu/ppc750/machine_check_on_bus_error")
{
    Machine m;
    m.s().msr = msr::ME;
    m.s().gpr[3] = 0x00100000u;  // outside the 64 KB RAM
    m.load(kCode, {lwz(4, 0, 3)});
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x200u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode);
    CHECK_EQ(m.s().msr & msr::ME, 0u);  // ME cleared on machine check
    CHECK(!m.cpu.checkstopped());
}

TEST_CASE("cpu/ppc750/checkstop_when_me_clear")
{
    Machine m;
    m.s().msr = 0;
    m.s().pc = 0x00200000u;  // instruction fetch from nowhere
    m.cpu.step();
    CHECK(m.cpu.checkstopped());
    const u64 before = m.cpu.instructions_executed();
    m.cpu.run(10);
    CHECK_EQ(m.cpu.instructions_executed(), before);  // stays stopped
}

// --- interrupts -------------------------------------------------------------------

TEST_CASE("cpu/ppc750/external_interrupt_respects_ee")
{
    Machine m;
    m.load(kCode, {li(3, 1), li(4, 2)});
    m.cpu.set_external_interrupt(true);
    m.cpu.step();  // EE = 0: not taken
    CHECK_EQ(m.s().pc, kCode + 4u);
    m.s().msr = msr::EE;
    m.cpu.step();  // taken before executing the next instruction
    CHECK_EQ(m.s().pc, 0x500u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode + 4u);
    CHECK_EQ(m.s().spr[spr::SRR1], msr::EE);
    CHECK_EQ(m.s().gpr[4], 0u);
}

TEST_CASE("cpu/ppc750/decrementer_interrupt")
{
    Machine m;
    m.load(kCode, {li(3, 1), li(4, 2), li(5, 3)});
    m.s().spr[spr::DEC] = 2;
    m.s().msr = msr::EE;
    m.cpu.advance_timebase(2);  // DEC 2 -> 0: no exception yet
    m.cpu.step();
    CHECK_EQ(m.s().pc, kCode + 4u);
    m.cpu.advance_timebase(1);  // DEC 0 -> 0xFFFFFFFF: bit 0 goes 0 -> 1
    m.cpu.step();
    CHECK_EQ(m.s().pc, 0x900u);
    CHECK_EQ(m.s().spr[spr::SRR0], kCode + 4u);
    // Taken once: after rfi-like return it does not fire again by itself.
    m.s().pc = kCode + 4;
    m.s().msr = msr::EE;
    m.cpu.step();
    CHECK_EQ(m.s().pc, kCode + 8u);
}
