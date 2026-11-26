// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the floating-point vectors in tests/vectors/dingusppc/ppcfloattests.csv
// through our PowerPC 750 interpreter.
//
// Vector semantics (same as the upstream test program): FPSCR = rounding
// mode (round=RTN/RTZ/RPI/RNI) or VE=1 (round=VEN); f4 = frA, f5 = frB,
// f6 = frC, f3 = 0, CR = 0; execute one instruction; compare f3 (not for
// FCMP*), FPSCR and CR. Operands are decimal strings or named constants.

#include "core/cpu/ppc750/ppc750.h"
#include "core/mem/flat_memory.h"
#include "tests/framework/g3test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace g3;

namespace {

u64 bits_of(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }

double parse_operand(const std::string& t)
{
    using dl = std::numeric_limits<double>;
    using fl = std::numeric_limits<float>;
    if (t == "snan") return dl::signaling_NaN();
    if (t == "qnan" || t == "nan") return dl::quiet_NaN();
    if (t == "-FLT_MAX") return -fl::max();
    if (t == "-FLT_MIN") return -fl::min();
    if (t == "-DBL_MAX") return -dl::max();
    if (t == "-DBL_MIN") return -dl::min();
    if (t == "FLT_MIN") return fl::min();
    if (t == "FLT_MAX") return fl::max();
    if (t == "DBL_MIN") return dl::min();
    if (t == "DBL_MAX") return dl::max();
    return std::strtod(t.c_str(), nullptr);
}

struct FVector {
    std::string mnemonic, round;
    u32 opcode = 0, fpscr = 0, cr = 0;
    u64 frd = 0, fra = 0, frb = 0, frc = 0;
    int line = 0;
};

std::vector<FVector> load(const char* path)
{
    std::vector<FVector> out;
    std::ifstream in(path);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#')
            continue;
        std::stringstream ss(line);
        std::string tok;
        FVector v;
        v.line = lineno;
        std::getline(ss, v.mnemonic, ',');
        std::getline(ss, tok, ',');
        v.opcode = static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16));
        while (std::getline(ss, tok, ',')) {
            const auto eq = tok.find('=');
            const std::string key = tok.substr(0, eq), val = tok.substr(eq + 1);
            if (key == "round") v.round = val.substr(0, 3);
            else if (key == "frD") v.frd = std::strtoull(val.c_str(), nullptr, 16);
            else if (key == "frA") v.fra = bits_of(parse_operand(val));
            else if (key == "frB") v.frb = bits_of(parse_operand(val));
            else if (key == "frC") v.frc = bits_of(parse_operand(val));
            else if (key == "FPSCR") v.fpscr = static_cast<u32>(std::strtoul(val.c_str(), nullptr, 16));
            else if (key == "CR") v.cr = static_cast<u32>(std::strtoul(val.c_str(), nullptr, 16));
        }
        out.push_back(v);
    }
    return out;
}

int run_vectors(const std::vector<FVector>& vectors, fp::Tininess tininess, bool verbose,
                std::map<std::string, int>* by_mnemonic)
{
    static u8 ram[4096];
    FlatMemory mem(ram, sizeof ram);
    ppc::Ppc750 cpu(mem);
    cpu.set_fp_tininess(tininess);
    int failures = 0, printed = 0;
    for (const FVector& v : vectors) {
        cpu.reset();
        auto& s = cpu.state();
        s.pc = 0x1000;
        s.msr = ppc::msr::FP;
        u32 fpscr = 0;
        if (v.round == "RTN") fpscr = 0;
        else if (v.round == "RTZ") fpscr = 1;
        else if (v.round == "RPI") fpscr = 2;
        else if (v.round == "RNI") fpscr = 3;
        else if (v.round == "VEN") fpscr = ppc::fpscr::VE;
        s.fpscr = fpscr;
        s.fpr[3] = 0;
        s.fpr[4] = v.fra;
        s.fpr[5] = v.frb;
        s.fpr[6] = v.frc;
        s.cr = 0;
        cpu.execute(v.opcode);

        const bool is_cmp = v.mnemonic.rfind("FCMP", 0) == 0;
        const bool ok = (is_cmp || s.fpr[3] == v.frd) && s.fpscr == v.fpscr && s.cr == v.cr;
        if (!ok) {
            ++failures;
            if (by_mnemonic)
                ++(*by_mnemonic)[v.mnemonic];
            if (verbose && printed++ < 25)
                std::fprintf(stderr,
                             "  line %d %s %s: A=%016llX B=%016llX C=%016llX expected "
                             "frD=%016llX FPSCR=%08X CR=%08X, got frD=%016llX FPSCR=%08X CR=%08X\n",
                             v.line, v.mnemonic.c_str(), v.round.c_str(),
                             (unsigned long long)v.fra, (unsigned long long)v.frb,
                             (unsigned long long)v.frc, (unsigned long long)v.frd, v.fpscr, v.cr,
                             (unsigned long long)s.fpr[3], s.fpscr, s.cr);
        }
    }
    return failures;
}

} // namespace

TEST_CASE("cpu/vectors/floating_point")
{
    const auto vectors = load(G3_SOURCE_DIR "/tests/vectors/dingusppc/ppcfloattests.csv");
    REQUIRE(vectors.size() == 2054);
    std::map<std::string, int> by;
    const int failures = run_vectors(vectors, fp::Tininess::BeforeRounding, true, &by);
    for (const auto& [name, count] : by)
        std::fprintf(stderr, "  %-8s %d failure(s)\n", name.c_str(), count);
    std::fprintf(stderr, "  FP vectors: %zu run, %d failed\n", vectors.size(), failures);
    const int after = run_vectors(vectors, fp::Tininess::AfterRounding, false, nullptr);
    std::fprintf(stderr, "  (with tininess after rounding instead: %d failed)\n", after);
    CHECK_EQ(failures, 0);
}
