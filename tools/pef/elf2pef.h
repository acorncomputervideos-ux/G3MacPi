// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/pef/elf2pef.h — converts a position-independent PowerPC ELF shared
// object (powerpc-linux-gnu-gcc -fPIC -shared) into a PEF container.
//
// Layout: the loaded image (all PT_LOAD segments, from address 0) becomes
// ONE PEF "executable data" section (kind 6): SVR4 PIC code reaches its
// data relative to its own address, so code and data must stay together.
// Dynamic relocations become PEF relocations:
//   R_PPC_RELATIVE (22)               word = addend;      + section address
//   R_PPC_ADDR32 (1), R_PPC_GLOB_DAT (20) to a defined symbol
//                                     word = value + addend; + section address
//   ... to an undefined symbol listed as an import
//                                     word = addend;      + the import's address
//   R_PPC_JMP_SLOT (21) and anything else: rejected (calls to Mac OS go
//   through guest/cfm glue, never through a PLT).
// Exports: data symbols as they are; code symbols get a transition vector
// {code address, TOC} appended to the section (both words relocated by
// the section address), and the export points at it (class TVector).
// Sources: ELF32 (System V ABI), PowerPC ELF ABI relocation numbers, and
// the PEF facts in pef_writer.h. See docs/tools/pef.md.

#ifndef G3_TOOLS_PEF_ELF2PEF_H
#define G3_TOOLS_PEF_ELF2PEF_H

#include "tools/pef/pef_writer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace g3::pef {

struct ElfToPefOptions {
    struct Exp {
        std::string symbol;
        bool code = true;  // true: export a transition vector; false: a data symbol
    };
    struct Imp {
        std::string symbol;   // the ELF symbol (e.g. imp_SynchronizeIO)
        std::string library;  // e.g. DriverServicesLib
        std::string name;     // the PEF imported name (e.g. SynchronizeIO)
        SymbolClass cls = kTVectorSymbol;
        bool weak = false;
    };
    std::vector<Exp> exports;
    std::vector<Imp> imports;
    ShareKind share = kProcessShare;
};

// Returns the PEF container; on error an empty vector and *error.
// If `container` is given, it receives the intermediate description.
std::vector<uint8_t> elf_to_pef(const std::vector<uint8_t>& elf, const ElfToPefOptions& opt,
                                std::string* error, Container* container = nullptr);

} // namespace g3::pef

#endif // G3_TOOLS_PEF_ELF2PEF_H
