// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-checks the two independent decoders: the interpreter (ppc750*.cpp)
// and the disassembler (disasm.cpp). Every encoding the disassembler accepts
// as a valid 750 instruction must not be rejected as illegal by the
// interpreter. (The converse is allowed for encodings that differ from valid
// ones only in reserved bits, which the CPU executes like the valid form.)

#include "core/cpu/ppc750/disasm.h"
#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"

#include <cstdio>

using namespace g3;
using namespace g3::ppc;

TEST_CASE("cpu/decode_consistency/disasm_vs_interpreter")
{
    static u8 ram[64 * 1024];
    FlatMemory mem(ram, sizeof ram);
    Ppc750 cpu(mem);

    u64 x = 0x243F6A8885A308D3ull;
    int mismatches = 0, lenient = 0, valid = 0;
    for (int i = 0; i < 1000000; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        u32 insn = static_cast<u32>(x);
        // Bias towards the populated opcode groups 19, 31, 59, 63.
        static const u32 kHot[4] = {19, 31, 59, 63};
        if ((x >> 40) & 1)
            insn = (insn & 0x03FFFFFFu) | (kHot[(x >> 41) & 3] << 26);

        char text[96];
        const bool dis_ok = disassemble(0x1000, insn, text, sizeof text);

        cpu.reset();
        auto& s = cpu.state();
        s.pc = 0x1000;
        s.msr = msr::FP | msr::ME;   // supervisor, FP available
        for (u32 r = 0; r < 32; ++r)
            s.gpr[r] = 0x2000;       // keep effective addresses inside RAM
        s.spr[spr::SRR1] = 0;
        cpu.execute(insn);
        const bool illegal = s.pc == 0x700 && (s.spr[spr::SRR1] & srr1::ILLEGAL);

        if (dis_ok) {
            ++valid;
            if (illegal && mismatches++ < 20)
                std::fprintf(stderr, "  %08X '%s' accepted by disassembler, illegal in CPU\n",
                             insn, text);
        } else if (!illegal) {
            ++lenient;
        }
    }
    std::fprintf(stderr,
                 "  1000000 random words: %d valid 750 instructions; %d executed by the CPU "
                 "although shown as dc.l (reserved bits set)\n",
                 valid, lenient);
    CHECK_EQ(mismatches, 0);
}
