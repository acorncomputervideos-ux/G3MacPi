// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/pef/elf2pef_main.cpp — command line for elf2pef:
//   g3_elf2pef in.so out.pef [--export NAME[:data]]... [--import SYMBOL=Library:Name[:weak]]...
// Exports are code (transition vectors) unless marked :data.

#include "tools/pef/elf2pef.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.so out.pef [--export NAME[:data]] [--import SYM=Lib:Name[:weak]]\n", argv[0]);
        return 2;
    }
    g3::pef::ElfToPefOptions opt;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--export" && i + 1 < argc) {
            std::string v = argv[++i];
            g3::pef::ElfToPefOptions::Exp e;
            if (v.size() > 5 && v.compare(v.size() - 5, 5, ":data") == 0) {
                e.code = false;
                v.resize(v.size() - 5);
            }
            e.symbol = v;
            opt.exports.push_back(e);
        } else if (a == "--import" && i + 1 < argc) {
            const std::string v = argv[++i];
            const size_t eq = v.find('='), colon = v.find(':', eq);
            if (eq == std::string::npos || colon == std::string::npos) {
                std::fprintf(stderr, "bad --import %s\n", v.c_str());
                return 2;
            }
            g3::pef::ElfToPefOptions::Imp imp;
            imp.symbol = v.substr(0, eq);
            imp.library = v.substr(eq + 1, colon - eq - 1);
            imp.name = v.substr(colon + 1);
            if (imp.name.size() > 5 && imp.name.compare(imp.name.size() - 5, 5, ":weak") == 0) {
                imp.weak = true;
                imp.name.resize(imp.name.size() - 5);
            }
            opt.imports.push_back(imp);
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> elf;
    for (int ch; (ch = std::fgetc(f)) != EOF;)
        elf.push_back(static_cast<uint8_t>(ch));
    std::fclose(f);
    std::string err;
    const std::vector<uint8_t> pef = g3::pef::elf_to_pef(elf, opt, &err);
    if (pef.empty()) {
        std::fprintf(stderr, "elf2pef: %s\n", err.c_str());
        return 1;
    }
    FILE* o = std::fopen(argv[2], "wb");
    if (!o || std::fwrite(pef.data(), 1, pef.size(), o) != pef.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::fclose(o);
    return 0;
}
