// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750.h — PowerPC 750 (G3) instruction interpreter.
//
// Milestone 2, step 1: integer, compare, logical, rotate/shift, condition
// register, branch, load/store (integer), trap, system call, rfi, SPR/MSR/SR
// access, and exception entry. Floating point (Milestone 2b) and address
// translation (Milestone 3) are not implemented yet: FP instructions raise
// FP-unavailable if MSR[FP]=0 and are treated as illegal otherwise, and
// addresses are used as physical addresses.
//
// Not supported yet (documented in docs/hardware/ppc750-cpu.md):
//  * little-endian mode (MSR[LE]); eciwx/ecowx; trace exceptions;
//    alignment exceptions other than lwarx/stwcx.; performance monitor.

#ifndef G3_CORE_CPU_PPC750_PPC750_H
#define G3_CORE_CPU_PPC750_PPC750_H

#include "core/common/types.h"
#include "core/cpu/ppc750/cpu_state.h"
#include "core/cpu/ppc750/memory_interface.h"

namespace g3::ppc {

class Ppc750 {
public:
    explicit Ppc750(MemoryInterface& memory, u32 pvr = kPvrMpc750Placeholder);

    // Hard reset (750 UM 2.1.4): MSR = IP, PC = 0xFFF00100, other state 0.
    void reset();

    // Fetches and executes one instruction at state().pc, then delivers a
    // pending external/decrementer interrupt if enabled.
    void step();

    // Executes 'count' instructions (fewer if the CPU checkstops).
    void run(u64 count);

    // Executes one already-fetched instruction as if it were at state().pc.
    // Used by tests and (later) by the debugger.
    void execute(u32 insn);

    // Interrupt inputs.
    void set_external_interrupt(bool asserted) { external_pending_ = asserted; }
    // Advances the time base and decrementer by 'ticks' time-base ticks.
    // A decrementer exception becomes pending when DEC bit 0 changes 0 -> 1.
    void advance_timebase(u32 ticks);

    CpuState& state() { return s_; }
    const CpuState& state() const { return s_; }

    // Checkstop: a machine check occurred with MSR[ME] = 0. The CPU stops.
    bool checkstopped() const { return checkstop_; }
    u64 instructions_executed() const { return instr_count_; }

    // Exception entry (public for tests and for device-driven exceptions).
    // 'srr0' is the address saved; 'srr1_cause' are the cause bits
    // OR-ed into SRR1 next to the copied MSR bits.
    void raise_exception(Exception type, u32 srr0, u32 srr1_cause = 0);

private:
    // --- dispatch (ppc750.cpp)
    void execute_op19(u32 insn);
    void execute_op31(u32 insn);
    void illegal(u32 insn);
    void privileged();
    bool in_user_mode() const { return (s_.msr & msr::PR) != 0; }

    // --- integer (ppc750_integer.cpp)
    void update_cr0(u32 result);
    void set_ov(bool overflow);
    void set_ca(bool carry) { s_.xer() = carry ? (s_.xer() | xer::CA) : (s_.xer() & ~xer::CA); }
    bool ca() const { return (s_.xer() & xer::CA) != 0; }
    void op_add_family(u32 insn, u32 xo);
    void op_mul_div(u32 insn, u32 xo);
    void op_logical(u32 insn, u32 xo);
    void op_shift(u32 insn, u32 xo);
    void op_compare(u32 crf, u32 a, u32 b, bool is_signed);
    void op_rotate(u32 insn, u32 opcode);
    void op_trap(u32 to, u32 a, u32 b);

    // --- branch and condition register (ppc750_branch.cpp)
    void op_b(u32 insn);
    void op_bc(u32 insn);
    void op_bclr_bcctr(u32 insn, bool to_ctr);
    void op_cr_logical(u32 insn, u32 xo);
    bool cr_bit(u32 bit) const { return (s_.cr >> (31u - bit)) & 1u; }
    void set_cr_bit(u32 bit, bool value);
    void set_cr_field(u32 field, u32 value);

    // --- load/store (ppc750_loadstore.cpp)
    u32 ea_d(u32 insn) const;   // (rA|0) + EXTS(d)
    u32 ea_x(u32 insn) const;   // (rA|0) + rB
    bool load_store_d(u32 insn, u32 opcode);
    bool load_store_x(u32 insn, u32 xo);
    bool op_lmw_stmw(u32 insn, bool store);
    bool op_string(u32 insn, bool store, u32 nbytes, u32 ea);
    bool op_lwarx(u32 insn);
    bool op_stwcx(u32 insn);
    bool op_dcbz(u32 insn);
    // Memory helpers: on failure they raise a machine check and return false.
    bool mem_read8(u32 a, u32& v);
    bool mem_read16(u32 a, u32& v);
    bool mem_read32(u32 a, u32& v);
    bool mem_write8(u32 a, u32 v);
    bool mem_write16(u32 a, u32 v);
    bool mem_write32(u32 a, u32 v);
    void machine_check();

    // --- system (ppc750_system.cpp)
    void op_mfspr(u32 insn);
    void op_mtspr(u32 insn);
    void op_mftb(u32 insn);
    static bool spr_implemented(u32 n);
    void op_rfi();
    void op_sc();

    MemoryInterface& mem_;
    CpuState s_;
    u32 pvr_;
    u32 next_pc_ = 0;        // set by execute(); exceptions/branches overwrite
    bool external_pending_ = false;
    bool dec_pending_ = false;
    bool checkstop_ = false;
    u64 instr_count_ = 0;
};

// Instruction field helpers (PowerPC bit numbering, PEM Appendix A).
namespace field {
constexpr u32 opcd(u32 i) { return i >> 26; }
constexpr u32 rd(u32 i) { return (i >> 21) & 31; }    // also rS, TO, BO, crbD
constexpr u32 ra(u32 i) { return (i >> 16) & 31; }    // also BI, crbA
constexpr u32 rb(u32 i) { return (i >> 11) & 31; }    // also SH, crbB, NB
constexpr u32 mb(u32 i) { return (i >> 6) & 31; }
constexpr u32 me(u32 i) { return (i >> 1) & 31; }
constexpr u32 xo10(u32 i) { return (i >> 1) & 0x3FF; }  // X/XL/XFX-form extended opcode
constexpr u32 xo9(u32 i) { return (i >> 1) & 0x1FF; }   // XO-form (without OE)
constexpr bool rc(u32 i) { return (i & 1u) != 0; }
constexpr bool oe(u32 i) { return (i & 0x400u) != 0; }
constexpr bool lk(u32 i) { return (i & 1u) != 0; }
constexpr bool aa(u32 i) { return (i & 2u) != 0; }
constexpr u32 simm(u32 i) { return static_cast<u32>(static_cast<s32>(static_cast<s16>(i & 0xFFFF))); }
constexpr u32 uimm(u32 i) { return i & 0xFFFF; }
constexpr u32 crfd(u32 i) { return (i >> 23) & 7; }
constexpr u32 crfs(u32 i) { return (i >> 18) & 7; }
} // namespace field

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_PPC750_H
