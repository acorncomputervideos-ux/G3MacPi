// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/cpu_selftest.h — the CPU self-test the Pi runs from its
// menu (docs/cpu/selftest.md).
//
// 1. Correctness: random PowerPC programs (self-modifying, over the
//    exception vectors, real mode and BAT-translated, with interrupts
//    arriving mid-block) run on the interpreter and on the block cache with
//    generated code; after every slice, registers, instruction counts and
//    all memory must be identical. The same check as the host's
//    cpu/jit/differential tests, but here the generated AArch64 code runs
//    natively.
// 2. Speed: a fixed benchmark loop (loads, stores, arithmetic, compares,
//    calls, translated through a BAT) timed with the interpreter, the block
//    cache, and the block cache with generated code.
//
// Freestanding; the caller supplies all memory and the clock.

#ifndef G3_PLATFORM_COMMON_CPU_SELFTEST_H
#define G3_PLATFORM_COMMON_CPU_SELFTEST_H

#include "core/common/types.h"

namespace g3 {

struct CpuSelfTest {
    static constexpr u32 kRam = 1u << 20;               // per test machine
    static constexpr u32 kArena = 4u << 20;             // block cache
    static constexpr u32 kWorkBytes = 2 * kRam + kArena;

    struct Setup {
        u8* work = nullptr;             // kWorkBytes
        u32* code = nullptr;            // executable memory for generated code (nullptr: none)
        u32 code_words = 0;
        // How generated code is run (nullptr: called directly, AArch64 only).
        // (the type of ppc::Ppc750::JitRunner; the header stays free of the CPU's)
        void (*runner)(void* code, void* cpu, void* context) = nullptr;
        void* runner_context = nullptr;
        u64 (*clock_us)(void* context) = nullptr;  // for the speed part (nullptr: skipped)
        void* clock_context = nullptr;
        u32 programs = 200;             // random programs for the correctness part
        u64 bench_instructions = 20000000;
    };

    struct Result {
        bool jit = false;               // generated code was available and used
        u32 programs = 0, failed = 0;
        u32 first_failed_program = 0;
        char first_failure[64] = {};    // what differed first
        u64 instructions = 0;           // compared, per machine
        u64 jit_block_runs = 0;
        // Speed in units of 0.01 MIPS: interpreter, block cache, generated code (0 = not run).
        u32 mips_x100[3] = {};
        bool ok() const { return programs > 0 && failed == 0; }
    };

    // Runs both parts. Takes a few seconds on a Pi 400 (more on a simulator).
    static Result run(const Setup& setup);

    // One-line summaries for a screen or a log ("CPU self-test: PASS ...").
    static void describe(const Result& r, char* line1, u32 cap1, char* line2, u32 cap2);
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_CPU_SELFTEST_H
