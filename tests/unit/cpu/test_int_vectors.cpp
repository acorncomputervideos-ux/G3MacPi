// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the integer test vectors in tests/vectors/dingusppc/ppcinttests.csv
// through our PowerPC 750 interpreter.
//
// Vector semantics (same as the upstream test program): r3 = rA, r4 = rB,
// XER = CR = 0, execute the one instruction, then compare r3 (not for CMP*),
// XER and CR with the expected values.

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace g3;

namespace {

struct Vector {
    std::string mnemonic;
    u32 opcode = 0, rd = 0, ra = 0, rb = 0, xer = 0, cr = 0;
    int line = 0;
};

std::vector<Vector> load_vectors(const char* path)
{
    std::vector<Vector> out;
    std::ifstream in(path);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#')
            continue;
        std::stringstream ss(line);
        std::string tok;
        Vector v;
        v.line = lineno;
        std::getline(ss, v.mnemonic, ',');
        std::getline(ss, tok, ',');
        v.opcode = static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16));
        while (std::getline(ss, tok, ',')) {
            const auto eq = tok.find('=');
            const std::string key = tok.substr(0, eq);
            const u32 val = static_cast<u32>(std::strtoul(tok.c_str() + eq + 1, nullptr, 16));
            if (key == "rD") v.rd = val;
            else if (key == "rA") v.ra = val;
            else if (key == "rB") v.rb = val;
            else if (key == "XER") v.xer = val;
            else if (key == "CR") v.cr = val;
        }
        out.push_back(v);
    }
    return out;
}

} // namespace

TEST_CASE("cpu/vectors/integer")
{
    const auto vectors = load_vectors(G3_SOURCE_DIR "/tests/vectors/dingusppc/ppcinttests.csv");
    REQUIRE(vectors.size() == 5620);

    static u8 ram[4096];
    FlatMemory mem(ram, sizeof ram);
    ppc::Ppc750 cpu(mem);

    std::map<std::string, int> failures_by_mnemonic;
    int failures = 0, printed = 0;
    for (const Vector& v : vectors) {
        cpu.reset();
        auto& s = cpu.state();
        s.pc = 0x1000;
        s.msr = 0;  // supervisor, real mode
        s.gpr[3] = v.ra;
        s.gpr[4] = v.rb;
        s.xer() = 0;
        s.cr = 0;
        cpu.execute(v.opcode);

        const bool is_cmp = v.mnemonic.rfind("CMP", 0) == 0;
        const bool ok = (is_cmp || s.gpr[3] == v.rd) && s.xer() == v.xer && s.cr == v.cr &&
                        s.pc == 0x1004;
        if (!ok) {
            ++failures;
            ++failures_by_mnemonic[v.mnemonic];
            if (printed++ < 20)
                std::fprintf(stderr,
                             "  vector line %d %s %08X: rA=%08X rB=%08X expected rD=%08X "
                             "XER=%08X CR=%08X, got rD=%08X XER=%08X CR=%08X pc=%08X\n",
                             v.line, v.mnemonic.c_str(), v.opcode, v.ra, v.rb, v.rd, v.xer, v.cr,
                             s.gpr[3], s.xer(), s.cr, s.pc);
        }
    }
    for (const auto& [name, count] : failures_by_mnemonic)
        std::fprintf(stderr, "  %-8s %d failure(s)\n", name.c_str(), count);
    std::fprintf(stderr, "  integer vectors: %zu run, %d failed\n", vectors.size(), failures);
    CHECK_EQ(failures, 0);
}
