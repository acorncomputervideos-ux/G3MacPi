// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/firmware.cpp — see firmware.h.

#include "core/firmware/openfirm/firmware.h"

#include "core/firmware/openfirm/rtas.h"

namespace g3::of {

bool Firmware::install(u32 addr)
{
    if (!mem_.write32(addr, kTrapClientInterface) || !mem_.write32(addr + 4, kTrapIdle))
        return false;
    entry_ = addr;
    return true;
}

bool Firmware::install_vectors()
{
    for (u32 v = 0x100; v <= 0x1700; v += 0x100)
        if (!mem_.write32(v, kTrapVector | (v >> 8)))
            return false;
    return true;
}

void Firmware::start_client(ppc::CpuState& s, u32 entry, u32 stack_top) const
{
    s.pc = entry;
    s.gpr[1] = (stack_top - 64) & ~15u;  // 16-byte aligned, with a little headroom
    s.gpr[3] = 0;
    s.gpr[4] = 0;
    s.gpr[5] = entry_;
    s.msr &= ~(ppc::msr::IR | ppc::msr::DR | ppc::msr::EE | ppc::msr::IP);
    s.msr |= ppc::msr::FP | ppc::msr::ME;
    if (virtual_mode_)
        s.msr |= ppc::msr::IR | ppc::msr::DR;
}

bool Firmware::hle_trap(void* context, u32 insn, ppc::CpuState& s, u32& next_pc)
{
    auto* fw = static_cast<Firmware*>(context);
    if (insn == kTrapIdle) {
        next_pc = s.pc;  // a client that exited stays here
        return true;
    }
    if ((insn & ~0x1Fu) == kTrapVector) {
        fw->vector_ = (insn & 0x1F) << 8;
        fw->vector_srr0_ = s.spr[ppc::spr::SRR0];
        fw->vector_srr1_ = s.spr[ppc::spr::SRR1];
        fw->exited_ = true;
        next_pc = fw->entry_ + 4;  // parked
        return true;
    }
    if (insn == kTrapRtas && fw->rtas_) {
        // r3 = argument buffer, LR = return address (rtas.h).
        fw->rtas_->call(fw->mem_, s.gpr[3]);
        next_pc = s.lr();
        if (fw->exit_pending_) {
            fw->exit_pending_ = false;
            next_pc = fw->entry_ + 4;
        }
        return true;
    }
    if (insn != kTrapClientInterface)
        return false;  // not ours: the CPU takes a program exception
    ++fw->calls_;
    s.gpr[3] = fw->ci_.call(s.gpr[3]);
    next_pc = s.lr();  // return to the caller, like the blr of a real entry
    if (fw->exit_pending_) {
        fw->exit_pending_ = false;
        next_pc = fw->entry_ + 4;  // `exit` doesn't return
    }
    return true;
}

void Firmware::on_exit(void* context)
{
    auto* fw = static_cast<Firmware*>(context);
    fw->exited_ = true;
    fw->exit_pending_ = true;
}

} // namespace g3::of
