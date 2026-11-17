// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/firmware.cpp — see firmware.h.

#include "core/firmware/openfirm/firmware.h"

namespace g3::of {

bool Firmware::install(u32 addr)
{
    if (!mem_.write32(addr, kTrapClientInterface))
        return false;
    entry_ = addr;
    return true;
}

bool Firmware::hle_trap(void* context, u32 insn, ppc::CpuState& s, u32& next_pc)
{
    auto* fw = static_cast<Firmware*>(context);
    if (insn != kTrapClientInterface)
        return false;  // not ours: the CPU takes a program exception
    ++fw->calls_;
    s.gpr[3] = fw->ci_.call(s.gpr[3]);
    next_pc = s.lr();  // return to the caller, like the blr of a real entry
    return true;
}

} // namespace g3::of
