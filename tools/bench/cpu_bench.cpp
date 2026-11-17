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
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace g3;
using namespace asm_ppc;

int main(int argc, char** argv)
{
    static u8 ram[128 * 1024];
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

    // Optional second argument: translation mode.
    //   real  (default) MSR[IR,DR] = 0
    //   bat   code and data mapped 1:1 by IBAT0/DBAT0 (128 KB blocks)
    //   pages code and data mapped 1:1 by page-table entries
    const char* mode = argc > 2 ? argv[2] : "real";
    auto& s = cpu.state();
    if (std::strcmp(mode, "bat") == 0) {
        s.spr[ppc::spr::IBAT0U] = 0x2;  // EA 0, 128 KB, Vs
        s.spr[ppc::spr::IBAT0U + 1] = 0x2;
        s.spr[ppc::spr::DBAT0U] = 0x2;
        s.spr[ppc::spr::DBAT0U + 1] = 0x2;
        s.msr = ppc::msr::IR | ppc::msr::DR;
    } else if (std::strcmp(mode, "pages") == 0) {
        // 64 KB hashed page table at 0x0 is not possible (code lives there),
        // so use RAM 0x10000.. as the table: SDR1 = 0x00010000, VSID 0.
        // Map pages 0x1000 (code) and 0x8000 (data) 1:1.
        s.spr[ppc::spr::SDR1] = 0x00010000;
        s.sr[0] = 0;
        for (u32 page : {0x1000u, 0x8000u}) {
            const u32 pidx = page >> 12;          // hash = 0 ^ pidx
            const u32 pteg = 0x10000 + (pidx << 6);
            mem.write32(pteg, 0x80000000u);       // V, VSID 0, H 0, API 0
            mem.write32(pteg + 4, page | 2);      // RPN, PP = RW
        }
        s.msr = ppc::msr::IR | ppc::msr::DR;
    }

    // Optional argument: number of guest instructions (default 100 million).
    const u64 kInstructions = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100'000'000ull;
    const auto t0 = std::chrono::steady_clock::now();
    cpu.run(kInstructions);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("[%s] %llu guest instructions in %.3f s = %.1f million instructions/s "
                "(%.2f ns each)\n",
                mode, static_cast<unsigned long long>(cpu.instructions_executed()), secs,
                cpu.instructions_executed() / secs / 1e6, secs * 1e9 / cpu.instructions_executed());
    return 0;
}
