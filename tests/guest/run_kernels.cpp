// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/guest/run_kernels.cpp — runs guest/tests/kernels.c, compiled with
// the PowerPC cross-compiler, on the emulated 750, and compares every
// result with the same C code compiled natively for the host.
//
// Built only when powerpc-linux-gnu-gcc is available (CMake option); the
// guest ELF is placed with our own ELF init-program (CHRP binding rules).

#include "core/cpu/ppc750/ppc750.h"
#include "core/firmware/openfirm/elf_loader.h"
#include "core/mem/physical_bus.h"
#include "guest/tests/kernels.h"

#include <cstdio>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kRam = 1u << 20;
constexpr u32 kImage = 0x80000;      // where the ELF file is loaded ("load-base")
constexpr u32 kStack = 0xF0000;
constexpr u32 kStopWord = 0x00004F99u;

bool stop_trap(void* ctx, u32 insn, ppc::CpuState&, u32&)
{
    if (insn != kStopWord)
        return false;
    *static_cast<bool*>(ctx) = true;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s kernels.elf\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<u8> file;
    for (int c; (c = std::fgetc(f)) != EOF;)
        file.push_back(static_cast<u8>(c));
    std::fclose(f);

    std::vector<u8> ram(kRam);
    PhysicalBus bus;
    bus.map_ram(0, ram.data(), kRam);
    if (file.size() > kRam - kImage) {
        std::fprintf(stderr, "ELF too large\n");
        return 2;
    }
    for (size_t i = 0; i < file.size(); ++i)
        ram[kImage + i] = file[i];
    of::ElfProgram prog;
    const of::ElfError e = of::load_elf_program(bus, nullptr, kImage, static_cast<u32>(file.size()), prog);
    if (e != of::ElfError::None) {
        std::fprintf(stderr, "ELF rejected (error %d)\n", static_cast<int>(e));
        return 1;
    }

    ppc::Ppc750 cpu(bus);
    bool stopped = false;
    cpu.set_hle_handler(stop_trap, &stopped);

    const u32 args[][2] = {
        {0, 0}, {1, 2}, {10, 3}, {47, 9}, {123, 456}, {1000, 77}, {65535, 255},
        {0x7FFFFFFF, 3}, {0x80000000u, 7}, {0xFFFFFFFFu, 0xFFFFFFFFu}, {12345, 99}, {255, 100},
    };
    // Arguments each kernel accepts with defined C behaviour (no out-of-range
    // float-to-int conversions, bounded loops).
    auto usable = [](u32 k, u32 a, u32) {
        if (k == 0) return a <= 1000;                // fib: loop count
        if (k == 1 || k == 7) return true;
        if (k == 2) return true;                     // n is clamped to 256
        if (k == 5) return a <= 100000;              // float: keep the result in int range
        return true;
    };
    int failures = 0, runs = 0;
    for (u32 k = 0; k < K_COUNT; ++k) {
        for (const auto& ab : args) {
            u32 a = ab[0], b = ab[1];
            if (k == 1 || k == 2 || k == 7)
                b = b % 300;                         // lengths
            if (!usable(k, a, b))
                continue;
            const u32 expect = k_table[k](a, b);
            bus.write32(K_CMD_ADDR + 0, k);
            bus.write32(K_CMD_ADDR + 4, a);
            bus.write32(K_CMD_ADDR + 8, b);
            bus.write32(K_CMD_ADDR + 12, 0xDEADBEEFu);
            cpu.reset();
            ppc::CpuState& s = cpu.state();
            s.msr = ppc::msr::FP;                    // real mode, FP enabled, vectors at 0
            s.pc = prog.entry;
            s.gpr[1] = kStack;
            stopped = false;
            u64 steps = 0;
            while (!stopped && !cpu.checkstopped() && steps < 50000000ull) {
                cpu.step();
                ++steps;
            }
            u32 got = 0;
            bus.read32(K_CMD_ADDR + 12, got);
            ++runs;
            if (!stopped || got != expect) {
                ++failures;
                std::printf("FAIL kernel %u (a=0x%08X b=0x%08X): emulated 0x%08X native 0x%08X%s pc=0x%08X\n",
                            k, a, b, got, expect, stopped ? "" : " (did not finish)", s.pc);
            }
        }
    }
    std::printf("guest kernels: %d runs, %d failures\n", runs, failures);
    return failures ? 1 : 0;
}
