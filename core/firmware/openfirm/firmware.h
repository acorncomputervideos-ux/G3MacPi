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
    // Where a client that called `exit` is parked (the word after the entry):
    // it spins there. Real firmware would return to its user interface.
    static constexpr u32 kTrapIdle = kTrapBase | 0x02;

    Firmware(DeviceTree& tree, ppc::MemoryInterface& mem) : mem_(mem), ci_(tree, mem)
    {
        ci_.set_exit(on_exit, this);
    }
    // True once the client has called `exit`.
    bool client_exited() const { return exited_; }

    // Writes the entry stub at 'addr' (4 bytes, big-endian). False if the
    // memory isn't writable there.
    bool install(u32 addr);
    u32 entry() const { return entry_; }

    // Routes the CPU's HLE traps to this firmware.
    void attach(ppc::Ppc750& cpu) { cpu.set_hle_handler(hle_trap, this); }

    ClientInterface& client() { return ci_; }

    // "go": starts a client program at `entry` (real mode). Registers as
    // the bindings specify (docs/firmware/elf-loader.md): r1 = a stack
    // given by the firmware, r3 = r4 = 0 (CHRP 10.2), r5 = the client
    // interface entry (PowerPC binding; to verify), MSR IR/DR/EE clear.
    // In Virtual-Mode (set_virtual_mode) the client starts with MSR IR/DR
    // set instead, under the firmware's translations (PowerPC binding §4.2).
    void start_client(ppc::CpuState& s, u32 entry, u32 stack_top) const;
    void set_virtual_mode(bool v) { virtual_mode_ = v; }
    u32 calls() const { return calls_; }

private:
    static bool hle_trap(void* context, u32 insn, ppc::CpuState& s, u32& next_pc);
    static void on_exit(void* context);

    ppc::MemoryInterface& mem_;
    ClientInterface ci_;
    u32 entry_ = 0;
    u32 calls_ = 0;
    bool virtual_mode_ = false;
    bool exited_ = false;
    bool exit_pending_ = false;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_FIRMWARE_H
