// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/firmware.h — joins the device tree and the client
// interface to the CPU.
//
// A client program reaches Open Firmware by calling the client-interface
// entry address it was given (in r5 at entry, per the PowerPC binding of
// IEEE 1275), with the argument array address in r3; the result comes
// back in r3 and control returns to the caller's link register.
//
// Our entry point is one emulator-defined trap word in guest memory
// (primary opcode 0, illegal on the 750, so no real code uses it). The
// CPU's HLE hook runs the client interface in C++ and returns to LR.
// Which memory the trap word lives in, and whether calls come with
// address translation on, is decided by the boot code (to verify by
// logging Mac OS's calls); for now, argument addresses are physical.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_FIRMWARE_H
#define G3_CORE_FIRMWARE_OPENFIRM_FIRMWARE_H

#include "core/cpu/ppc750/ppc750.h"
#include "core/firmware/openfirm/client_interface.h"
#include "core/firmware/openfirm/device_tree.h"

namespace g3::of {

class Firmware {
public:
    // Emulator-defined trap words: primary opcode 0, 0x4F ('O') in bits
    // 15-8, a service number in bits 7-0.
    static constexpr u32 kTrapBase = 0x00004F00u;
    static constexpr u32 kTrapClientInterface = kTrapBase | 0x01;

    Firmware(DeviceTree& tree, ppc::MemoryInterface& mem) : mem_(mem), ci_(tree, mem) {}

    // Writes the entry stub at 'addr' (4 bytes, big-endian). False if the
    // memory isn't writable there.
    bool install(u32 addr);
    u32 entry() const { return entry_; }

    // Routes the CPU's HLE traps to this firmware.
    void attach(ppc::Ppc750& cpu) { cpu.set_hle_handler(hle_trap, this); }

    ClientInterface& client() { return ci_; }
    u32 calls() const { return calls_; }

private:
    static bool hle_trap(void* context, u32 insn, ppc::CpuState& s, u32& next_pc);

    ppc::MemoryInterface& mem_;
    ClientInterface ci_;
    u32 entry_ = 0;
    u32 calls_ = 0;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_FIRMWARE_H
