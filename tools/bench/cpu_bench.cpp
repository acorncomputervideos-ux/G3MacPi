// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/bench/cpu_bench.cpp — interpreter throughput micro-benchmark.
//
// Runs a fixed integer loop (add, rotate, logical, load/store, compare,
// branch) and reports guest instructions per second. Host-side numbers are
// only for tracking relative changes; the Raspberry Pi 400 is the target
// that counts (docs/04-roadmap.md, Milestone 3).

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <chrono>
#include <cstdio>

using namespace g3;
using namespace asm_ppc;

int main()
{
    static u8 ram[64 * 1024];
    FlatMemory mem(ram, sizeof ram);
    ppc::Ppc750 cpu(mem);

    // r3 = 0x8000 (data), CTR = iterations. Loop body: 8 instructions.
    const u32 code[] = {
        lis(3, 0), ori(3, 3, 0x8000), lis(4, 0x00FF), ori(4, 4, 0xFFFF), mtspr(ppc::spr::CTR, 4),
        // loop:
        add(5, 5, 6),              // 1
        rlwnm(6, 5, 7, 0, 31),     // 2
        ori(7, 6, 0x1234),         // 3
        stw(5, 0, 3),              // 4
        lwz(8, 0, 3),              // 5
        cmpw(0, 8, 5),             // 6
        addi(6, 6, 3),             // 7
        bdnz(-28),                 // 8
    };
    u32 addr = 0x1000;
    for (u32 w : code) {
        mem.write32(addr, w);
        addr += 4;
    }
    cpu.state().pc = 0x1000;
    cpu.state().msr = 0;

    constexpr u64 kInstructions = 100'000'000;
    const auto t0 = std::chrono::steady_clock::now();
    cpu.run(kInstructions);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%llu guest instructions in %.3f s = %.1f million instructions/s "
                "(%.2f ns each)\n",
                static_cast<unsigned long long>(cpu.instructions_executed()), secs,
                cpu.instructions_executed() / secs / 1e6, secs * 1e9 / cpu.instructions_executed());
    return 0;
}
