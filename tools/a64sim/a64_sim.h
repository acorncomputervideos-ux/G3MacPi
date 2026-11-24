// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/a64sim/a64_sim.h — a strict simulator for the AArch64 subset the
// JIT emits, so generated code can be run and checked on a non-ARM host
// (docs/cpu/jit-a64.md).
//
// Semantics follow the Arm ARM pseudocode for each instruction. Any
// encoding outside the subset is an error, not a guess. Memory accesses go
// straight to host memory (the generated code holds host addresses). BLR
// to an address outside the code range calls that host function natively
// as void(void*, u32) with x0, w1, then overwrites the caller-saved
// registers x0-x18 (as a real call may), so code that wrongly relies on
// them fails. The stack must be 16-byte aligned at every call.

#ifndef G3_TOOLS_A64SIM_A64_SIM_H
#define G3_TOOLS_A64SIM_A64_SIM_H

#include "core/common/types.h"

namespace g3::a64 {

class Simulator {
public:
    // Runs the function at `entry` with x0 = arg until it returns. Code is
    // fetched from [code_lo, code_hi). False on an error (see error()).
    bool call(const u32* entry, u64 arg, const void* code_lo, const void* code_hi);
    const char* error() const { return error_; }
    u32 error_pc_offset() const { return error_off_; }  // words from code_lo
    u64 executed() const { return executed_; }
    u64 host_calls() const { return host_calls_; }
    u64 jumps() const { return jumps_; }  // BRs taken (block to block)

private:
    bool fail(const char* why, const u32* pc, const void* lo);
    u64 x_[32] = {};   // x_[31]: SP
    bool n_ = false, z_ = false, c_ = false, v_ = false;  // NZCV
    alignas(16) u8 stack_[64 * 1024];
    const char* error_ = nullptr;
    u32 error_off_ = 0;
    u64 executed_ = 0, host_calls_ = 0, jumps_ = 0;
};

// Adapter for Ppc750::set_jit_runner: runs `code` with x0 = cpu.
void simulate_jit_block(void* code, void* cpu, void* context);
// AArch64 instructions simulate_jit_block has executed so far (all calls):
// the generated code's own work, without the handlers it calls.
u64 simulated_instructions();

} // namespace g3::a64

#endif // G3_TOOLS_A64SIM_A64_SIM_H
