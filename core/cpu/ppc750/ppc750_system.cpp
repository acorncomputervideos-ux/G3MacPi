// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/ppc750_system.cpp — special-purpose registers, time base,
// rfi and sc (PEM chapter 2.3 and chapter 12; 750 UM chapter 2).

#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

namespace {

// The SPR number is encoded with its two 5-bit halves swapped (PEM mfspr).
constexpr u32 spr_number(u32 insn)
{
    return ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
}

bool user_readable(u32 n)
{
    return n == spr::XER || n == spr::LR || n == spr::CTR ||
           (n >= spr::UMMCR0 && n <= spr::UPMC4);  // read-only user copies
}

bool user_writable(u32 n)
{
    return n == spr::XER || n == spr::LR || n == spr::CTR;
}

// Implemented XER bits: SO, OV, CA and the byte count (bits 25-31).
constexpr u32 kXerMask = 0xE000007Fu;

} // namespace

bool Ppc750::spr_implemented(u32 n)
{
    switch (n) {
    case spr::XER: case spr::LR: case spr::CTR:
    case spr::DSISR: case spr::DAR: case spr::DEC: case spr::SDR1:
    case spr::SRR0: case spr::SRR1:
    case spr::SPRG0: case spr::SPRG1: case spr::SPRG2: case spr::SPRG3:
    case spr::EAR: case spr::TBL_WRITE: case spr::TBU_WRITE: case spr::PVR:
    case spr::UMMCR0: case spr::UPMC1: case spr::UPMC2: case spr::USIA:
    case spr::UMMCR1: case spr::UPMC3: case spr::UPMC4:
    case spr::MMCR0: case spr::PMC1: case spr::PMC2: case spr::SIA:
    case spr::MMCR1: case spr::PMC3: case spr::PMC4:
    case spr::HID0: case spr::HID1: case spr::IABR: case spr::DABR:
    case spr::L2CR: case spr::ICTC:
    case spr::THRM1: case spr::THRM2: case spr::THRM3:
        return true;
    default:
        // IBAT0U..DBAT3L
        return n >= spr::IBAT0U && n <= spr::DBAT0U + 7;
    }
}

void Ppc750::op_mfspr(u32 insn)
{
    const u32 n = spr_number(insn);
    if (in_user_mode() && !user_readable(n)) {
        // Supervisor SPR from user mode: privileged exception, whether or
        // not the SPR exists (PEM 6.4.7).
        privileged();
        return;
    }
    // TBL/TBU are read with mftb; TBL_WRITE/TBU_WRITE are write-only.
    if (!spr_implemented(n) || n == spr::TBL_WRITE || n == spr::TBU_WRITE) {
        illegal(insn);
        return;
    }

    if (n == spr::DEC && time_hook_)
        time_hook_(time_ctx_);  // the decrementer as of this instruction
    u32 v;
    switch (n) {
    case spr::PVR: v = pvr_; break;
    // User-level read-only copies of the performance monitor registers.
    case spr::UMMCR0: v = s_.spr[spr::MMCR0]; break;
    case spr::UPMC1:  v = s_.spr[spr::PMC1]; break;
    case spr::UPMC2:  v = s_.spr[spr::PMC2]; break;
    case spr::USIA:   v = s_.spr[spr::SIA]; break;
    case spr::UMMCR1: v = s_.spr[spr::MMCR1]; break;
    case spr::UPMC3:  v = s_.spr[spr::PMC3]; break;
    case spr::UPMC4:  v = s_.spr[spr::PMC4]; break;
    case spr::THRM1:
    case spr::THRM2:  v = thermal_read(s_.spr[n]); s_.spr[n] = v; break;
    default:          v = s_.spr[n]; break;
    }
    s_.gpr[field::rd(insn)] = v;
}

// Thermal assist unit (MPC750 UM ch. 10): with THRM3[E] set, a THRMn
// with V set reports its comparison as valid (TIV) and TIN = 1 when the
// junction temperature is above the threshold (TID = 0) or below it
// (TID = 1). The comparison is ready at once and the temperature is a
// fixed kJunctionCelsius (our choice; no thermal interrupts). Mac OS 9.2.1
// scans thresholds downward and waits for TIV on each one.
u32 Ppc750::thermal_read(u32 thrm) const
{
    constexpr u32 TIN = 0x80000000u, TIV = 0x40000000u, TID = 0x4u, V = 0x1u, E = 0x1u;
    if (!(s_.spr[spr::THRM3] & E) || !(thrm & V))
        return thrm;
    const u32 threshold = (thrm >> 23) & 0x7F;
    const bool crossed = (thrm & TID) ? kJunctionCelsius < threshold : kJunctionCelsius > threshold;
    return (thrm & ~TIN) | TIV | (crossed ? TIN : 0);
}

void Ppc750::op_mtspr(u32 insn)
{
    const u32 n = spr_number(insn);
    if (in_user_mode() && !user_writable(n)) {
        privileged();
        return;
    }
    // The user copies of the performance monitor registers are read-only.
    if (!spr_implemented(n) || (n >= spr::UMMCR0 && n <= spr::UPMC4)) {
        illegal(insn);
        return;
    }

    const u32 v = s_.gpr[field::rd(insn)];
    if (n == spr::DEC) {
        if (time_hook_)
            time_hook_(time_ctx_);  // ticks up to now count against the old value
        break_run_ = true;          // the next expiry must be planned from the new one
    }
    switch (n) {
    case spr::XER:
        s_.xer() = v & kXerMask;
        break;
    case spr::PVR:
        // Read-only. Writes are ignored (behaviour on real hardware to be
        // verified; see docs/hardware/ppc750-cpu.md).
        break;
    case spr::TBL_WRITE:
        s_.tb = (s_.tb & 0xFFFFFFFF00000000ull) | v;
        break;
    case spr::TBU_WRITE:
        s_.tb = (s_.tb & 0x00000000FFFFFFFFull) | (static_cast<u64>(v) << 32);
        break;
    case spr::DEC:
        // The 750's decrementer exception is edge-triggered: it is signalled
        // when bit 0 changes from 0 to 1, by counting down or by this write
        // (QEMU models the 750 family so, "PPC_DECR_UNDERFLOW_TRIGGERED").
        // Mac OS's nanokernel relies on it: to re-raise an overdue timer it
        // writes 0x7FFF0000 and then a negative value (its idle loop at
        // physical 0xF19200 in Mac OS 9.2.1); without the exception every
        // timer stops. docs/hardware/ppc750-cpu.md.
        if (!(s_.spr[n] & 0x80000000u) && (v & 0x80000000u)) {
            dec_pending_ = true;
            exit_ = 1;
        }
        s_.spr[n] = v;
        break;
    default:
        s_.spr[n] = v;
        // Translation context changed: drop cached translations.
        if (n == spr::SDR1 || (n >= spr::IBAT0U && n <= spr::DBAT0U + 7))
            flush_tlb();
        break;
    }
}

void Ppc750::op_mftb(u32 insn)
{
    const u32 n = spr_number(insn);
    if (time_hook_)
        time_hook_(time_ctx_);  // the time base as of this instruction
    if (n == spr::TBL_READ)
        s_.gpr[field::rd(insn)] = static_cast<u32>(s_.tb);
    else if (n == spr::TBU_READ)
        s_.gpr[field::rd(insn)] = static_cast<u32>(s_.tb >> 32);
    else
        illegal(insn);
}

void Ppc750::op_rfi()
{
    s_.msr = s_.spr[spr::SRR1] & msr::kRfiMask;
    next_pc_ = s_.spr[spr::SRR0] & ~3u;
}

void Ppc750::op_sc()
{
    // SRR0 = address of the instruction after sc (PEM 6.4.10).
    raise_exception(Exception::SystemCall, s_.pc + 4);
}

} // namespace g3::ppc
