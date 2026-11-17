// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750.cpp — reset, fetch/execute loop, instruction
// dispatch and exception entry for the PowerPC 750 interpreter.

#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

Ppc750::Ppc750(MemoryInterface& memory, u32 pvr) : mem_(memory), pvr_(pvr)
{
    reset();
}

void Ppc750::reset()
{
    s_ = CpuState{};
    // 750 UM 2.1.4 / PEM 6.4.1: after hard reset MSR[IP] = 1 and execution
    // starts at the system reset vector, 0xFFF00100.
    s_.msr = msr::IP;
    s_.pc = 0xFFF00100u;
    s_.spr[spr::PVR] = pvr_;
    next_pc_ = s_.pc;
    external_pending_ = false;
    dec_pending_ = false;
    checkstop_ = false;
    instr_count_ = 0;
}

void Ppc750::raise_exception(Exception type, u32 srr0, u32 srr1_cause)
{
    // PEM 6.4: SRR0 = address to resume at; SRR1 = cause bits + MSR bits
    // 16-23, 25-27, 30-31. New MSR: everything cleared except IP and ME,
    // LE = ILE. A machine check additionally clears ME.
    s_.spr[spr::SRR0] = srr0;
    s_.spr[spr::SRR1] = (s_.msr & msr::kSrr1CopyMask) | srr1_cause;

    u32 new_msr = s_.msr & (msr::IP | msr::ME);
    if (s_.msr & msr::ILE)
        new_msr |= msr::LE;
    if (type == Exception::MachineCheck)
        new_msr &= ~msr::ME;
    s_.msr = new_msr;

    next_pc_ = ((s_.msr & msr::IP) ? 0xFFF00000u : 0u) | static_cast<u32>(type);
}

void Ppc750::machine_check()
{
    if (s_.msr & msr::ME) {
        raise_exception(Exception::MachineCheck, s_.pc);
    } else {
        // PEM 6.4.2: machine check with MSR[ME] = 0 -> checkstop.
        checkstop_ = true;
        next_pc_ = s_.pc;
    }
}

void Ppc750::illegal(u32 /*insn*/)
{
    raise_exception(Exception::Program, s_.pc, srr1::ILLEGAL);
}

void Ppc750::privileged()
{
    raise_exception(Exception::Program, s_.pc, srr1::PRIVILEGED);
}

void Ppc750::advance_timebase(u32 ticks)
{
    s_.tb += ticks;
    const u32 old_dec = s_.spr[spr::DEC];
    const u32 new_dec = old_dec - ticks;
    s_.spr[spr::DEC] = new_dec;
    // PEM 2.3.14 / 750 UM: exception when DEC bit 0 changes from 0 to 1.
    if (!(old_dec & 0x80000000u) && (new_dec & 0x80000000u))
        dec_pending_ = true;
}

void Ppc750::step()
{
    if (checkstop_)
        return;

    // Asynchronous interrupts are taken between instructions. SRR0 holds the
    // address of the instruction that would have executed next.
    if (s_.msr & msr::EE) {
        if (external_pending_) {
            raise_exception(Exception::External, s_.pc);
            s_.pc = next_pc_;
            return;
        }
        if (dec_pending_) {
            dec_pending_ = false;
            raise_exception(Exception::Decrementer, s_.pc);
            s_.pc = next_pc_;
            return;
        }
    }

    u32 insn;
    if (!mem_.fetch32(s_.pc, insn)) {
        machine_check();
        s_.pc = next_pc_;
        return;
    }
    execute(insn);
}

void Ppc750::run(u64 count)
{
    for (u64 i = 0; i < count && !checkstop_; ++i)
        step();
}

void Ppc750::execute(u32 insn)
{
    next_pc_ = s_.pc + 4;
    ++instr_count_;

    const u32 op = field::opcd(insn);
    switch (op) {
    case 3:  // twi
        op_trap(field::rd(insn), s_.gpr[field::ra(insn)], field::simm(insn));
        break;
    case 7:   // mulli
    case 8:   // subfic
    case 12:  // addic
    case 13:  // addic.
    case 14:  // addi
    case 15:  // addis
        op_add_family(insn, 0);
        break;
    case 10:  // cmpli
        op_compare(field::crfd(insn), s_.gpr[field::ra(insn)], field::uimm(insn), false);
        break;
    case 11:  // cmpi
        op_compare(field::crfd(insn), s_.gpr[field::ra(insn)], field::simm(insn), true);
        break;
    case 16:
        op_bc(insn);
        break;
    case 17:  // sc (bit 30 must be 1)
        if ((insn & 2u) == 0) {
            illegal(insn);
            break;
        }
        op_sc();
        break;
    case 18:
        op_b(insn);
        break;
    case 19:
        execute_op19(insn);
        break;
    case 20:  // rlwimi
    case 21:  // rlwinm
    case 23:  // rlwnm
        op_rotate(insn, op);
        break;
    case 24: case 25: case 26: case 27: case 28: case 29:  // ori .. andis.
        op_logical(insn, 0);
        break;
    case 31:
        execute_op31(insn);
        break;
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45:
        load_store_d(insn, op);
        break;
    case 46:  // lmw
        op_lmw_stmw(insn, false);
        break;
    case 47:  // stmw
        op_lmw_stmw(insn, true);
        break;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:  // FP load/store
    case 59: case 63:                                                          // FP arithmetic
        // FPU: Milestone 2b. Until then the FP-unavailable check is correct
        // and everything else is reported as illegal.
        if (!(s_.msr & msr::FP))
            raise_exception(Exception::FpUnavailable, s_.pc);
        else
            illegal(insn);
        break;
    default:
        illegal(insn);
        break;
    }

    s_.pc = next_pc_;
}

void Ppc750::execute_op19(u32 insn)
{
    switch (field::xo10(insn)) {
    case 0: {  // mcrf
        const u32 src = (s_.cr >> (28 - 4 * field::crfs(insn))) & 0xF;
        set_cr_field(field::crfd(insn), src);
        break;
    }
    case 16:  // bclr
        op_bclr_bcctr(insn, false);
        break;
    case 528:  // bcctr
        op_bclr_bcctr(insn, true);
        break;
    case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
        op_cr_logical(insn, field::xo10(insn));
        break;
    case 50:  // rfi
        if (in_user_mode())
            privileged();
        else
            op_rfi();
        break;
    case 150:  // isync: no instruction prefetch to discard in an interpreter
        break;
    default:
        illegal(insn);
        break;
    }
}

void Ppc750::execute_op31(u32 insn)
{
    const u32 xo10 = field::xo10(insn);

    // XO-form arithmetic (bit 21 = OE, so match on the 9-bit opcode).
    switch (field::xo9(insn)) {
    case 8: case 10: case 40: case 104: case 136: case 138: case 200: case 202:
    case 232: case 234: case 266:
        op_add_family(insn, field::xo9(insn));
        return;
    case 235: case 459: case 491:  // mullw, divwu, divw
        op_mul_div(insn, field::xo9(insn));
        return;
    default:
        break;
    }

    switch (xo10) {
    case 11: case 75:  // mulhwu, mulhw (no OE form)
        op_mul_div(insn, xo10);
        break;
    case 0:  // cmp
        op_compare(field::crfd(insn), s_.gpr[field::ra(insn)], s_.gpr[field::rb(insn)], true);
        break;
    case 32:  // cmpl
        op_compare(field::crfd(insn), s_.gpr[field::ra(insn)], s_.gpr[field::rb(insn)], false);
        break;
    case 4:  // tw
        op_trap(field::rd(insn), s_.gpr[field::ra(insn)], s_.gpr[field::rb(insn)]);
        break;
    case 26: case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
    case 922: case 954:  // cntlzw and andc nor eqv xor orc or nand extsh extsb
        op_logical(insn, xo10);
        break;
    case 24: case 536: case 792: case 824:  // slw srw sraw srawi
        op_shift(insn, xo10);
        break;
    case 19:  // mfcr
        s_.gpr[field::rd(insn)] = s_.cr;
        break;
    case 144: {  // mtcrf
        const u32 crm = (insn >> 12) & 0xFF;
        u32 mask = 0;
        for (u32 f = 0; f < 8; ++f)
            if (crm & (0x80u >> f))
                mask |= 0xF0000000u >> (4 * f);
        s_.cr = (s_.cr & ~mask) | (s_.gpr[field::rd(insn)] & mask);
        break;
    }
    case 512: {  // mcrxr
        set_cr_field(field::crfd(insn), s_.xer() >> 28);
        s_.xer() &= 0x0FFFFFFFu;
        break;
    }
    case 83:  // mfmsr
        if (in_user_mode())
            privileged();
        else
            s_.gpr[field::rd(insn)] = s_.msr;
        break;
    case 146:  // mtmsr
        if (in_user_mode())
            privileged();
        else
            s_.msr = s_.gpr[field::rd(insn)];
        break;
    case 339:
        op_mfspr(insn);
        break;
    case 467:
        op_mtspr(insn);
        break;
    case 371:
        op_mftb(insn);
        break;
    case 595:  // mfsr
    case 659:  // mfsrin
    case 210:  // mtsr
    case 242:  // mtsrin
        if (in_user_mode()) {
            privileged();
            break;
        } else {
            const u32 sr = (xo10 == 595 || xo10 == 210) ? ((insn >> 16) & 0xF)
                                                         : (s_.gpr[field::rb(insn)] >> 28);
            if (xo10 == 595 || xo10 == 659)
                s_.gpr[field::rd(insn)] = s_.sr[sr];
            else
                s_.sr[sr] = s_.gpr[field::rd(insn)];
        }
        break;
    case 306:  // tlbie (MMU: Milestone 3)
    case 566:  // tlbsync
    case 470:  // dcbi
        if (in_user_mode())
            privileged();
        break;
    case 54: case 86: case 246: case 278: case 982:  // dcbst dcbf dcbtst dcbt icbi
    case 598: case 854:                                // sync eieio
        // No caches are modelled; these have no architecturally visible
        // effect here. (icbi will matter once decoded-instruction caching
        // or a JIT exists.)
        break;
    case 1014:
        op_dcbz(insn);
        break;
    case 20:
        op_lwarx(insn);
        break;
    case 150:
        op_stwcx(insn);
        break;
    case 533: {  // lswx
        const u32 n = s_.xer() & xer::kByteCountMask;
        op_string(insn, false, n, ea_x(insn));
        break;
    }
    case 661: {  // stswx
        const u32 n = s_.xer() & xer::kByteCountMask;
        op_string(insn, true, n, ea_x(insn));
        break;
    }
    case 597: {  // lswi
        const u32 nb = field::rb(insn);
        op_string(insn, false, nb == 0 ? 32 : nb, field::ra(insn) ? s_.gpr[field::ra(insn)] : 0);
        break;
    }
    case 725: {  // stswi
        const u32 nb = field::rb(insn);
        op_string(insn, true, nb == 0 ? 32 : nb, field::ra(insn) ? s_.gpr[field::ra(insn)] : 0);
        break;
    }
    case 535: case 567: case 599: case 631: case 663: case 695: case 727: case 759: case 983:
        // FP indexed loads/stores (Milestone 2b).
        if (!(s_.msr & msr::FP))
            raise_exception(Exception::FpUnavailable, s_.pc);
        else
            illegal(insn);
        break;
    default:
        if (!load_store_x(insn, xo10))
            illegal(insn);
        break;
    }
}

} // namespace g3::ppc
