// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/pef/pef_writer.h — builds PEF containers (Apple's Preferred
// Executable Format), for guest code such as our Mac OS display driver.
// A host-side build tool, not part of the emulator.
//
// Source: Inside Macintosh: Mac OS Runtime Architectures (1997), chapter 8
// "PEF Structure"; bit layouts of the relocation instructions, the hash
// functions and the hash slot/exported-symbol encodings from Apple's
// PEFBinaryFormat.h (facts only). See docs/tools/pef.md.
//
// Supported subset: code (0), unpacked data (1) and constant (3) sections,
// one loader section; imported libraries and symbols; relocations that add
// an imported symbol's address or a section's address to a word; exports;
// main/init/term entry points. Pattern-initialized data is not produced.

#ifndef G3_TOOLS_PEF_PEF_WRITER_H
#define G3_TOOLS_PEF_PEF_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

namespace g3::pef {

enum SectionKind : uint8_t { kCode = 0, kUnpackedData = 1, kPatternData = 2, kConstant = 3, kLoader = 4 };
enum ShareKind : uint8_t { kProcessShare = 1, kGlobalShare = 4, kProtectedShare = 5 };
enum SymbolClass : uint8_t { kCodeSymbol = 0, kDataSymbol = 1, kTVectorSymbol = 2, kTocSymbol = 3, kGlueSymbol = 4 };

struct Section {
    std::string name;            // "" = no name
    SectionKind kind = kCode;
    ShareKind share = kGlobalShare;
    uint8_t alignment = 4;       // power of 2 (4 = 16 bytes)
    std::vector<uint8_t> data;   // initialized contents
    uint32_t total_size = 0;     // >= data.size(); the rest is zero-initialized (data sections)
};

struct ImportedSymbol {
    std::string name;
    SymbolClass cls = kTVectorSymbol;
    bool weak = false;
};

struct ImportedLibrary {
    std::string name;
    uint32_t old_imp_version = 0, current_version = 0;
    bool weak = false;
    bool init_before = false;
    std::vector<ImportedSymbol> symbols;
};

// "Add the address of <target> to the 32-bit word at <offset> in <section>."
struct Relocation {
    enum Target : uint8_t { kImport, kSectionAddress };
    uint32_t section = 0;
    uint32_t offset = 0;         // must be a multiple of 4
    Target target = kImport;
    uint32_t index = 0;          // import index (over all libraries) or section number
};

struct Export {
    std::string name;
    SymbolClass cls = kTVectorSymbol;
    int16_t section = 0;         // section number; -2 = absolute value
    uint32_t value = 0;          // offset within the section
};

struct EntryPoint {
    int32_t section = -1;        // -1 = none
    uint32_t offset = 0;
};

struct Container {
    uint32_t date_time_stamp = 0;  // seconds since 1904-01-01
    uint32_t old_def_version = 0, old_imp_version = 0, current_version = 0;
    std::vector<Section> sections;  // code/data/constant only; the loader section is added
    std::vector<ImportedLibrary> imports;
    std::vector<Relocation> relocations;
    std::vector<Export> exports;
    EntryPoint main, init, term;
};

// The PEF hash word of a name, and the hash table index of a hash word.
uint32_t hash_word(const std::string& name);
uint32_t hash_index(uint32_t hash_word, uint32_t table_power);
uint32_t hash_table_power(uint32_t export_count);

// Builds the container. On error returns an empty vector and sets *error.
std::vector<uint8_t> build(const Container& c, std::string* error);

} // namespace g3::pef

#endif // G3_TOOLS_PEF_PEF_WRITER_H
