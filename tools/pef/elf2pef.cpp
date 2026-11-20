// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/pef/elf2pef.cpp — see elf2pef.h.

#include "tools/pef/elf2pef.h"

#include <algorithm>
#include <map>

namespace g3::pef {

namespace {

constexpr uint32_t PT_LOAD = 1, SHT_RELA = 4, SHT_DYNSYM = 11;
constexpr uint32_t R_PPC_NONE = 0, R_PPC_ADDR32 = 1, R_PPC_GLOB_DAT = 20, R_PPC_JMP_SLOT = 21,
                   R_PPC_RELATIVE = 22;

struct Reader {
    const std::vector<uint8_t>& b;
    bool ok = true;
    uint32_t u8(size_t at) { if (at >= b.size()) { ok = false; return 0; } return b[at]; }
    uint32_t u16(size_t at) { return (u8(at) << 8) | u8(at + 1); }
    uint32_t u32(size_t at) { return (u16(at) << 16) | u16(at + 2); }
    std::string str(size_t at)
    {
        std::string s;
        for (; at < b.size() && b[at]; ++at)
            s.push_back(static_cast<char>(b[at]));
        if (at >= b.size())
            ok = false;
        return s;
    }
};

void put32(std::vector<uint8_t>& v, uint32_t at, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v[at + i] = static_cast<uint8_t>(x >> (24 - 8 * i));
}

} // namespace

std::vector<uint8_t> elf_to_pef(const std::vector<uint8_t>& elf, const ElfToPefOptions& opt, std::string* error,
                                Container* out)
{
    auto fail = [&](const std::string& s) {
        if (error)
            *error = s;
        return std::vector<uint8_t>{};
    };
    Reader r{elf};
    if (elf.size() < 52 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
        return fail("not an ELF file");
    if (elf[4] != 1 || elf[5] != 2)
        return fail("not a 32-bit big-endian ELF file");
    if (r.u16(16) != 3)
        return fail("not a shared object (link with -shared)");
    if (r.u16(18) != 20)
        return fail("not a PowerPC ELF file");

    // --- the loaded image -------------------------------------------------------------
    const uint32_t phoff = r.u32(28), phentsize = r.u16(42), phnum = r.u16(44);
    uint32_t image_size = 0;
    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t p = phoff + static_cast<size_t>(i) * phentsize;
        if (r.u32(p) != PT_LOAD)
            continue;
        image_size = std::max(image_size, r.u32(p + 8) + r.u32(p + 20));
    }
    if (!r.ok || image_size == 0)
        return fail("no loadable segments");
    std::vector<uint8_t> image(image_size, 0);
    uint32_t file_end = 0;  // end of initialized contents
    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t p = phoff + static_cast<size_t>(i) * phentsize;
        if (r.u32(p) != PT_LOAD)
            continue;
        const uint32_t off = r.u32(p + 4), vaddr = r.u32(p + 8), filesz = r.u32(p + 16);
        if (static_cast<uint64_t>(off) + filesz > elf.size())
            return fail("segment outside the file");
        for (uint32_t k = 0; k < filesz; ++k)
            image[vaddr + k] = elf[off + k];
        file_end = std::max(file_end, vaddr + filesz);
    }

    // --- dynamic symbols and relocations (through the section headers) ------------------
    const uint32_t shoff = r.u32(32), shentsize = r.u16(46), shnum = r.u16(48);
    struct Sym { std::string name; uint32_t value; uint16_t shndx; };
    std::vector<Sym> syms;
    std::vector<size_t> rela_sections;
    for (uint32_t i = 0; i < shnum; ++i) {
        const size_t s = shoff + static_cast<size_t>(i) * shentsize;
        const uint32_t type = r.u32(s + 4);
        if (type == SHT_RELA)
            rela_sections.push_back(s);
        if (type != SHT_DYNSYM)
            continue;
        const uint32_t off = r.u32(s + 16), size = r.u32(s + 20), link = r.u32(s + 24);
        const size_t str = r.u32(shoff + static_cast<size_t>(link) * shentsize + 16);
        for (uint32_t k = 0; k + 16 <= size; k += 16)
            syms.push_back({r.str(str + r.u32(off + k)), r.u32(off + k + 4), static_cast<uint16_t>(r.u16(off + k + 14))});
    }
    if (!r.ok)
        return fail("malformed section headers or symbol table");

    Container c;
    std::map<std::string, uint32_t> import_index;  // ELF symbol -> import number
    {
        std::map<std::string, size_t> lib_of;
        for (const auto& imp : opt.imports) {
            if (!lib_of.count(imp.library)) {
                lib_of[imp.library] = c.imports.size();
                ImportedLibrary l;
                l.name = imp.library;
                c.imports.push_back(l);
            }
            c.imports[lib_of[imp.library]].symbols.push_back({imp.name, imp.cls, imp.weak});
        }
        // Import numbers count across libraries in order.
        uint32_t n = 0;
        for (const auto& l : c.imports)
            for (const auto& s : l.symbols) {
                for (const auto& imp : opt.imports)
                    if (imp.library == l.name && imp.name == s.name)
                        import_index[imp.symbol] = n;
                ++n;
            }
    }

    std::vector<Relocation> relocs;
    for (size_t s : rela_sections) {
        const uint32_t off = r.u32(s + 16), size = r.u32(s + 20);
        for (uint32_t k = 0; k + 12 <= size; k += 12) {
            const uint32_t where = r.u32(off + k), info = r.u32(off + k + 4), addend = r.u32(off + k + 8);
            const uint32_t type = info & 0xFF, sym = info >> 8;
            if (type == R_PPC_NONE)
                continue;
            if (where % 4 || where + 4 > file_end)
                return fail("relocation outside the initialized image");
            if (type == R_PPC_RELATIVE) {
                put32(image, where, addend);
                relocs.push_back({0, where, Relocation::kSectionAddress, 0});
                continue;
            }
            if (type == R_PPC_ADDR32 || type == R_PPC_GLOB_DAT) {
                if (sym >= syms.size())
                    return fail("relocation names a missing symbol");
                const Sym& y = syms[sym];
                if (y.shndx != 0) {  // defined here
                    put32(image, where, y.value + addend);
                    relocs.push_back({0, where, Relocation::kSectionAddress, 0});
                } else {
                    auto it = import_index.find(y.name);
                    if (it == import_index.end())
                        return fail("undefined symbol '" + y.name + "' is not a declared import");
                    put32(image, where, addend);
                    relocs.push_back({0, where, Relocation::kImport, it->second});
                }
                continue;
            }
            if (type == R_PPC_JMP_SLOT) {
                const std::string n = sym < syms.size() ? syms[sym].name : "?";
                return fail("direct call to '" + n + "': call Mac OS routines through cfm_call");
            }
            return fail("unsupported relocation type " + std::to_string(type));
        }
    }
    if (!r.ok)
        return fail("malformed relocation section");

    // --- exports ------------------------------------------------------------------------
    auto find_sym = [&](const std::string& n) -> const Sym* {
        for (const Sym& y : syms)
            if (y.name == n && y.shndx != 0)
                return &y;
        return nullptr;
    };
    // Transition vectors go after the whole image, bss included (stored as
    // explicit zeros when there are any exports to append).
    std::vector<uint8_t> init(image.begin(), image.begin() + file_end);
    bool any_code_export = false;
    for (const auto& e : opt.exports)
        any_code_export |= e.code;
    if (any_code_export)
        init.resize((image_size + 7) & ~7u, 0);
    for (const auto& e : opt.exports) {
        const Sym* y = find_sym(e.symbol);
        if (!y)
            return fail("export '" + e.symbol + "' is not a defined symbol");
        if (!e.code) {
            c.exports.push_back({e.symbol, kDataSymbol, 0, y->value});
            continue;
        }
        // Transition vector {code, TOC}: both relocated by the section address.
        const uint32_t tv = static_cast<uint32_t>(init.size());
        init.resize(tv + 8, 0);
        put32(init, tv, y->value);
        put32(init, tv + 4, 0);
        relocs.push_back({0, tv, Relocation::kSectionAddress, 0});
        relocs.push_back({0, tv + 4, Relocation::kSectionAddress, 0});
        c.exports.push_back({e.symbol, kTVectorSymbol, 0, tv});
    }

    Section sec;
    sec.name = "image";
    sec.kind = kExecutableData;
    sec.share = opt.share;
    sec.data = init;
    sec.total_size = std::max<uint32_t>(static_cast<uint32_t>(init.size()), image_size);  // + bss
    c.sections = {sec};
    c.relocations = relocs;
    if (out)
        *out = c;
    return build(c, error);
}

} // namespace g3::pef
