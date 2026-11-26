// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks the disassembler against tests/vectors/dingusppc/ppcdisasmtest.csv.
//
// The vectors were written for a disassembler that also knows POWER/601 and
// 603 instructions. On the 750 those encodings are illegal, so for them we
// require "dc.l" instead (the same view the interpreter takes).

#include "core/cpu/ppc750/disasm.h"
#include "tests/framework/g3test.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace g3;

namespace {

std::string rtrim(std::string s)
{
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

const std::set<std::string> kNot750 = {
    // POWER / 601-only
    "abs", "abs.", "abso", "abso.", "clcs", "div", "div.", "divo", "divo.", "divs", "divs.",
    "divso", "divso.", "doz", "doz.", "dozi", "dozo", "dozo.", "maskg", "maskir", "mul", "mul.",
    "mulo", "mulo.", "nabs", "nabs.", "nabso", "nabso.", "rlmi", "rlmi.", "rrib", "rrib.", "sle",
    "sle.", "sleq", "sleq.", "sliq", "slliq", "slliq.", "sllq", "sllq.", "sre", "sre.", "sreq",
    "sreq.", "sriq", "sriq.",
    // 603-only software TLB loads; tlbia is optional and not on the 750
    "tlbld", "tlbli", "tlbia"};

} // namespace

TEST_CASE("cpu/vectors/disassembler")
{
    std::ifstream in(G3_SOURCE_DIR "/tests/vectors/dingusppc/ppcdisasmtest.csv");
    std::string line;
    int lineno = 0, run = 0, failures = 0, not750 = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> tok;
        std::stringstream ss(line);
        std::string t;
        while (std::getline(ss, t, ','))
            tok.push_back(t);
        REQUIRE(tok.size() >= 3);
        const u32 addr = static_cast<u32>(std::strtoul(tok[0].c_str(), nullptr, 16));
        const u32 insn = static_cast<u32>(std::strtoul(tok[1].c_str(), nullptr, 16));

        // Expected text, formatted like the disassembler: mnemonic padded
        // to 8 columns, operands joined by ", ".
        std::string expected = tok[2];
        if (kNot750.count(tok[2])) {
            ++not750;
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08X", insn);
            expected = std::string("dc.l    ") + hex;
        } else if (tok.size() > 3) {
            expected.append(tok[2].size() < 8 ? 8 - tok[2].size() : 1, ' ');
            for (size_t i = 3; i < tok.size(); ++i) {
                if (i > 3)
                    expected += ", ";
                expected += tok[i];
            }
        }

        char out[128];
        ppc::disassemble(addr, insn, out, sizeof out);
        ++run;
        if (rtrim(out) != rtrim(expected)) {
            if (failures++ < 30)
                std::fprintf(stderr, "  line %d %08X: expected '%s' got '%s'\n", lineno, insn,
                             expected.c_str(), out);
        }
    }
    std::fprintf(stderr, "  disassembler vectors: %d run (%d not on the 750 -> dc.l), %d failed\n",
                 run, not750, failures);
    CHECK_EQ(run, 397);
    CHECK_EQ(failures, 0);
}

TEST_CASE("cpu/disasm/misc_and_truncation")
{
    char out[64];
    CHECK(ppc::disassemble(0, 0x7C0802A6u, out, sizeof out));  // mflr r0
    CHECK(std::string(out) == "mflr    r0");
    CHECK(ppc::disassemble(0, 0x7C7A03A6u, out, sizeof out));  // mtspr SRR0,r3
    CHECK(std::string(out) == "mtspr   SRR0, r3");
    CHECK(ppc::disassemble(0, 0x3C60FFF0u, out, sizeof out));  // lis r3,-0x10
    CHECK(std::string(out) == "lis     r3, -0x10");
    CHECK(ppc::disassemble(0, 0x4C000064u, out, sizeof out));
    CHECK(std::string(out) == "rfi");
    CHECK(ppc::disassemble(0, 0x44000002u, out, sizeof out));
    CHECK(std::string(out) == "sc");
    CHECK(!ppc::disassemble(0, 0x00000000u, out, sizeof out));
    CHECK(std::string(out) == "dc.l    0x00000000");
    // Output is truncated safely.
    char tiny[5];
    ppc::disassemble(0, 0x7C0802A6u, tiny, sizeof tiny);
    CHECK(std::string(tiny) == "mflr");
}
