// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/pef/pef_writer.cpp — see pef_writer.h.

#include "tools/pef/pef_writer.h"

#include <algorithm>
#include <map>

namespace g3::pef {

namespace {

struct Out {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
    void align(size_t n) { while (b.size() % n) u8(0); }
    void put32(size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
    }
};

constexpr uint32_t fourcc(const char* s)
{
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

bool fail(std::string* error, const std::string& what)
{
    if (error)
        *error = what;
    return false;
}

// Relocation instructions for one section (2-byte blocks).
bool encode_relocations(std::vector<Relocation> rel, std::vector<uint16_t>& out, std::string* error)
{
    std::sort(rel.begin(), rel.end(), [](const Relocation& a, const Relocation& b) { return a.offset < b.offset; });
    uint32_t pos = 0;  // relocAddress, as an offset from the section start
    for (size_t i = 0; i < rel.size(); ++i) {
        const Relocation& r = rel[i];
        if (r.offset % 4)
            return fail(error, "relocation offset not word-aligned");
        if (i > 0 && r.offset == rel[i - 1].offset)
            return fail(error, "two relocations for the same word");
        if (r.offset != pos) {
            const uint32_t gap = r.offset - pos;
            if (r.offset > pos && gap <= 4096) {
                out.push_back(static_cast<uint16_t>(0x8000 | (gap - 1)));  // RelocIncrPosition
            } else {
                if (r.offset > 0x03FFFFFF)
                    return fail(error, "relocation offset too large");
                out.push_back(static_cast<uint16_t>(0xA000 | (r.offset >> 16)));  // RelocSetPosition
                out.push_back(static_cast<uint16_t>(r.offset & 0xFFFF));
            }
        }
        if (r.target == Relocation::kImport) {
            if (r.index <= 511) {
                out.push_back(static_cast<uint16_t>(0x6000 | (0 << 9) | r.index));  // RelocSmByImport
            } else {
                if (r.index > 0x03FFFFFF)
                    return fail(error, "import index too large");
                out.push_back(static_cast<uint16_t>(0xA400 | (r.index >> 16)));  // RelocLgByImport
                out.push_back(static_cast<uint16_t>(r.index & 0xFFFF));
            }
        } else {
            if (r.index > 511)
                return fail(error, "section index too large");
            out.push_back(static_cast<uint16_t>(0x6000 | (3 << 9) | r.index));  // RelocSmBySection
        }
        pos = r.offset + 4;  // every instruction above moves past the word it fixed
    }
    return true;
}

} // namespace

uint32_t hash_word(const std::string& name)
{
    // Inside Macintosh / PEFBinaryFormat.h: hash = rotate-ish(hash) ^ char,
    // with the "pseudo rotate" (x << 1) - (x >> 16) on a signed 32-bit value;
    // result = length << 16 | (hash ^ (hash >> 16)) & 0xFFFF.
    int32_t h = 0;
    uint32_t length = 0;
    for (unsigned char c : name) {
        if (c == 0)
            break;
        ++length;
        h = static_cast<int32_t>((static_cast<uint32_t>(h) << 1) - static_cast<uint32_t>(h >> 16)) ^ c;
    }
    return (length << 16) | (static_cast<uint32_t>(h ^ (h >> 16)) & 0xFFFF);
}

uint32_t hash_index(uint32_t w, uint32_t power)
{
    return (w ^ (w >> power)) & ((1u << power) - 1);
}

uint32_t hash_table_power(uint32_t count)
{
    // Smallest power (below 16) whose average chain length is under 10.
    uint32_t p = 0;
    while (p < 16 && (count >> p) >= 10)
        ++p;
    return p;
}

std::vector<uint8_t> build(const Container& c, std::string* error)
{
    auto err = [&](const std::string& s) {
        fail(error, s);
        return std::vector<uint8_t>{};
    };
    const uint32_t nsec = static_cast<uint32_t>(c.sections.size());
    for (const Section& s : c.sections) {
        if (s.kind != kCode && s.kind != kUnpackedData && s.kind != kConstant && s.kind != kExecutableData)
            return err("unsupported section kind");
        if (s.total_size && s.total_size < s.data.size())
            return err("total_size smaller than the data");
        // Read-only instantiated sections can't have zero-initialized extensions.
        if ((s.kind == kCode || s.kind == kConstant) && s.total_size > s.data.size())
            return err("read-only sections can't have zero-initialized space");
    }
    uint32_t import_count = 0;
    for (const ImportedLibrary& l : c.imports)
        import_count += static_cast<uint32_t>(l.symbols.size());
    for (const Relocation& r : c.relocations) {
        if (r.section >= nsec)
            return err("relocation in a missing section");
        if (r.offset + 4 > c.sections[r.section].data.size())
            return err("relocation outside the section's initialized data");
        if (r.target == Relocation::kImport && r.index >= import_count)
            return err("relocation names a missing import");
        if (r.target == Relocation::kSectionAddress && r.index >= nsec)
            return err("relocation names a missing section");
    }
    for (const Export& e : c.exports)
        if (e.section >= 0 && static_cast<uint32_t>(e.section) >= nsec)
            return err("export in a missing section");

    // --- loader section ---------------------------------------------------------------
    // Loader string table: library names, imported names, exported names.
    std::vector<uint8_t> strings;
    auto add_string = [&](const std::string& s) {
        const uint32_t off = static_cast<uint32_t>(strings.size());
        strings.insert(strings.end(), s.begin(), s.end());
        strings.push_back(0);
        return off;
    };
    Out ld;
    ld.b.resize(56);  // loader header, filled in below
    // Imported libraries, then imported symbols.
    std::vector<uint32_t> sym_entries;
    uint32_t first = 0;
    for (const ImportedLibrary& l : c.imports) {
        ld.u32(add_string(l.name));
        ld.u32(l.old_imp_version);
        ld.u32(l.current_version);
        ld.u32(static_cast<uint32_t>(l.symbols.size()));
        ld.u32(first);
        ld.u8((l.init_before ? 0x80 : 0) | (l.weak ? 0x40 : 0));
        ld.u8(0);
        ld.u16(0);
        for (const ImportedSymbol& s : l.symbols) {
            const uint32_t name = add_string(s.name);
            if (name > 0x00FFFFFF)
                return err("loader string table too large");
            sym_entries.push_back((static_cast<uint32_t>((s.weak ? 0x80 : 0) | s.cls) << 24) | name);
        }
        first += static_cast<uint32_t>(l.symbols.size());
    }
    for (uint32_t e : sym_entries)
        ld.u32(e);
    // Relocation headers, then the relocation area.
    std::map<uint32_t, std::vector<Relocation>> by_section;
    for (const Relocation& r : c.relocations)
        by_section[r.section].push_back(r);
    std::vector<std::vector<uint16_t>> code;
    for (auto& [sec, list] : by_section) {
        code.emplace_back();
        if (!encode_relocations(list, code.back(), error))
            return {};
    }
    uint32_t area_off = 0;
    size_t k = 0;
    for (auto& [sec, list] : by_section) {
        ld.u16(sec);
        ld.u16(0);
        ld.u32(static_cast<uint32_t>(code[k].size()));
        ld.u32(area_off);
        area_off += static_cast<uint32_t>(code[k].size() * 2);
        ++k;
    }
    const uint32_t reloc_instr_offset = static_cast<uint32_t>(ld.b.size());
    for (const auto& blocks : code)
        for (uint16_t b : blocks)
            ld.u16(b);
    // Exports: names into the string table first.
    struct Exp { const Export* e; uint32_t hash; uint32_t name; uint32_t slot; };
    std::vector<Exp> exps;
    const uint32_t power = hash_table_power(static_cast<uint32_t>(c.exports.size()));
    for (const Export& e : c.exports) {
        const uint32_t h = hash_word(e.name);
        exps.push_back({&e, h, add_string(e.name), hash_index(h, power)});
    }
    std::stable_sort(exps.begin(), exps.end(), [](const Exp& a, const Exp& b) { return a.slot < b.slot; });
    const uint32_t strings_offset = static_cast<uint32_t>(ld.b.size());
    ld.bytes(strings);
    ld.align(4);
    const uint32_t hash_offset = static_cast<uint32_t>(ld.b.size());
    // Hash table: per slot, (count << 18) | index of the first key.
    for (uint32_t slot = 0; slot < (1u << power); ++slot) {
        uint32_t count = 0, start = 0;
        for (size_t i = 0; i < exps.size(); ++i)
            if (exps[i].slot == slot) {
                if (count == 0)
                    start = static_cast<uint32_t>(i);
                ++count;
            }
        ld.u32((count << 18) | (count ? start : 0));
    }
    for (const Exp& x : exps)  // export key table
        ld.u32(x.hash);
    for (const Exp& x : exps) {  // exported symbol table (10 bytes each, packed)
        ld.u32((static_cast<uint32_t>(x.e->cls) << 24) | x.name);
        ld.u32(x.e->value);
        ld.u16(static_cast<uint16_t>(x.e->section));
    }
    // Loader header.
    Out hdr;
    hdr.u32(static_cast<uint32_t>(c.main.section));
    hdr.u32(c.main.offset);
    hdr.u32(static_cast<uint32_t>(c.init.section));
    hdr.u32(c.init.offset);
    hdr.u32(static_cast<uint32_t>(c.term.section));
    hdr.u32(c.term.offset);
    hdr.u32(static_cast<uint32_t>(c.imports.size()));
    hdr.u32(import_count);
    hdr.u32(static_cast<uint32_t>(by_section.size()));
    hdr.u32(reloc_instr_offset);
    hdr.u32(strings_offset);
    hdr.u32(hash_offset);
    hdr.u32(power);
    hdr.u32(static_cast<uint32_t>(c.exports.size()));
    std::copy(hdr.b.begin(), hdr.b.end(), ld.b.begin());

    // --- container --------------------------------------------------------------------
    const uint32_t total_sections = nsec + 1;  // + loader
    Out o;
    o.u32(fourcc("Joy!"));
    o.u32(fourcc("peff"));
    o.u32(fourcc("pwpc"));
    o.u32(1);  // formatVersion
    o.u32(c.date_time_stamp);
    o.u32(c.old_def_version);
    o.u32(c.old_imp_version);
    o.u32(c.current_version);
    o.u16(total_sections);
    o.u16(nsec);  // instantiated sections
    o.u32(0);
    // Section headers (containerOffset patched later), then the name table.
    std::vector<uint8_t> names;
    std::vector<size_t> header_at;
    auto name_off = [&](const std::string& n) -> uint32_t {
        if (n.empty())
            return 0xFFFFFFFFu;  // -1: no name
        const uint32_t off = static_cast<uint32_t>(names.size());
        names.insert(names.end(), n.begin(), n.end());
        names.push_back(0);
        return off;
    };
    for (uint32_t i = 0; i < total_sections; ++i) {
        const bool loader = i == nsec;
        const Section* s = loader ? nullptr : &c.sections[i];
        const uint32_t size = loader ? static_cast<uint32_t>(ld.b.size()) : static_cast<uint32_t>(s->data.size());
        header_at.push_back(o.b.size());
        o.u32(loader ? 0xFFFFFFFFu : name_off(s->name));
        o.u32(0);  // defaultAddress
        o.u32(loader ? 0 : std::max<uint32_t>(s->total_size, size));
        o.u32(loader ? 0 : size);  // unpackedSize
        o.u32(size);               // packedSize
        o.u32(0);                  // containerOffset (patched)
        o.u8(loader ? kLoader : s->kind);
        o.u8(loader ? 0 : s->share);
        o.u8(loader ? 0 : s->alignment);
        o.u8(0);
    }
    o.bytes(names);
    for (uint32_t i = 0; i < total_sections; ++i) {
        const bool loader = i == nsec;
        o.align(loader ? 4 : 16);  // unpacked code/data at least 16-byte aligned
        o.put32(header_at[i] + 20, static_cast<uint32_t>(o.b.size()));
        o.bytes(loader ? ld.b : c.sections[i].data);
    }
    if (error)
        error->clear();
    return o.b;
}

} // namespace g3::pef
