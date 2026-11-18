// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the PEF writer. A small "preparer" below follows Inside
// Macintosh's description of how the Code Fragment Manager places sections
// and executes relocation instructions, and looks up exports through the
// hash table; the writer's output must prepare correctly at any addresses.

#include "tests/framework/g3test.h"
#include "tools/pef/pef_writer.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace g3::pef;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

namespace {

u32 be32(const std::vector<u8>& b, size_t at)
{
    return (u32(b.at(at)) << 24) | (u32(b.at(at + 1)) << 16) | (u32(b.at(at + 2)) << 8) | b.at(at + 3);
}
u16 be16(const std::vector<u8>& b, size_t at) { return static_cast<u16>((b.at(at) << 8) | b.at(at + 1)); }

struct Prepared {
    bool ok = false;
    std::string why;
    std::map<u32, std::vector<u8>> mem;  // section number -> contents at run time
    std::vector<u32> addr;               // section number -> run-time address
    u32 word(u32 sec, u32 off) const { return be32(mem.at(sec), off); }
};

// Prepares a container: section i is placed at base[i]; imports resolve
// through `symbols` (name -> address).
Prepared prepare(const std::vector<u8>& c, const std::vector<u32>& base, const std::map<std::string, u32>& symbols)
{
    Prepared p;
    auto bad = [&](const char* w) { p.why = w; return p; };
    if (be32(c, 0) != 0x4A6F7921 || be32(c, 4) != 0x70656666 || be32(c, 8) != 0x70777063)
        return bad("tags");
    const u32 nsec = be16(c, 32), ninst = be16(c, 34);
    size_t loader = 0;
    bool have_loader = false;
    p.addr.assign(nsec, 0);
    for (u32 i = 0; i < nsec; ++i) {
        const size_t h = 40 + 28 * i;
        const u32 total = be32(c, h + 8), unpacked = be32(c, h + 12), packed = be32(c, h + 16), off = be32(c, h + 20);
        const u8 kind = c.at(h + 24);
        if (kind == kLoader) {
            loader = off;
            have_loader = true;
            continue;
        }
        if (i >= ninst)
            return bad("instantiated section after a non-instantiated one");
        std::vector<u8> m(total, 0);
        for (u32 k = 0; k < unpacked && k < packed; ++k)
            m[k] = c.at(off + k);
        p.mem[i] = m;
        p.addr[i] = base.at(i);
    }
    if (!have_loader)
        return bad("no loader section");
    // Loader header.
    const u32 nlib = be32(c, loader + 24), nimp = be32(c, loader + 28), nrel = be32(c, loader + 32);
    const u32 rel_off = be32(c, loader + 36), str_off = be32(c, loader + 40);
    std::vector<u32> import_addr;
    const size_t libs = loader + 56, syms = libs + 24 * nlib;
    for (u32 i = 0; i < nimp; ++i) {
        const u32 e = be32(c, syms + 4 * i);
        const std::string name(reinterpret_cast<const char*>(&c.at(loader + str_off + (e & 0xFFFFFF))));
        auto it = symbols.find(name);
        if (it == symbols.end())
            return bad("unresolved import");
        import_addr.push_back(it->second);
    }
    const size_t rel_headers = syms + 4 * nimp;
    for (u32 r = 0; r < nrel; ++r) {
        const size_t rh = rel_headers + 12 * r;
        const u32 sec = be16(c, rh), count = be32(c, rh + 4), first = be32(c, rh + 8);
        std::vector<u8>& m = p.mem.at(sec);
        // Relocation variables (initial values per Table 8-4).
        u32 reloc = 0, import_index = 0;
        u32 sect_c = nsec > 0 ? p.addr[0] : 0, sect_d = nsec > 1 ? p.addr[1] : 0;
        auto add = [&](u32 v) {
            const u32 w = be32(m, reloc) + v;
            for (int k = 0; k < 4; ++k) m.at(reloc + k) = static_cast<u8>(w >> (24 - 8 * k));
            reloc += 4;
        };
        const size_t ins = loader + rel_off + first;
        for (u32 blk = 0; blk < count;) {
            const u16 b = be16(c, ins + 2 * blk);
            ++blk;
            if ((b >> 14) == 0) {                         // RelocBySectDWithSkip
                reloc += 4 * ((b >> 6) & 0xFF);
                for (u32 k = 0; k < (b & 0x3F); ++k) add(sect_d);
            } else if ((b >> 13) == 2) {                   // Relocate Value group
                const u32 sub = (b >> 9) & 15, run = (b & 0x1FF) + 1;
                for (u32 k = 0; k < run; ++k) {
                    if (sub == 0) add(sect_c);
                    else if (sub == 1) add(sect_d);
                    else if (sub == 5) add(import_addr.at(import_index++));
                    else return bad("unexpected run subopcode");
                }
            } else if ((b >> 13) == 3) {                   // Relocate By Index group
                const u32 sub = (b >> 9) & 15, idx = b & 0x1FF;
                if (sub == 0) { add(import_addr.at(idx)); import_index = idx + 1; }
                else if (sub == 1) sect_c = p.addr.at(idx);
                else if (sub == 2) sect_d = p.addr.at(idx);
                else if (sub == 3) add(p.addr.at(idx));
                else return bad("unexpected index subopcode");
            } else if ((b >> 12) == 8) {                   // RelocIncrPosition
                reloc += (b & 0xFFF) + 1;
            } else if ((b >> 10) == 0x28 || (b >> 10) == 0x29) {  // SetPosition / LgByImport
                const u32 full = (u32(b & 0x3FF) << 16) | be16(c, ins + 2 * blk);
                ++blk;
                if ((b >> 10) == 0x28) reloc = full;
                else { add(import_addr.at(full)); import_index = full + 1; }
            } else {
                return bad("unexpected relocation opcode");
            }
        }
    }
    p.ok = true;
    return p;
}

// Export lookup through the hash table, as the spec describes it.
bool find_export(const std::vector<u8>& c, const std::string& name, u32& value, int16_t& section, u8& cls)
{
    const u32 nsec = be16(c, 32);
    size_t loader = 0;
    for (u32 i = 0; i < nsec; ++i)
        if (c.at(40 + 28 * i + 24) == kLoader)
            loader = be32(c, 40 + 28 * i + 20);
    const u32 str_off = be32(c, loader + 40), hash_off = be32(c, loader + 44);
    const u32 power = be32(c, loader + 48), nexp = be32(c, loader + 52);
    const u32 w = hash_word(name);
    const u32 slot = be32(c, loader + hash_off + 4 * hash_index(w, power));
    const u32 count = slot >> 18, start = slot & 0x3FFFF;
    const size_t keys = loader + hash_off + 4 * (1u << power), syms = keys + 4 * nexp;
    for (u32 i = start; i < start + count; ++i) {
        if (be32(c, keys + 4 * i) != w)
            continue;
        const size_t s = syms + 10 * i;
        const u32 cn = be32(c, s);
        if (std::string(reinterpret_cast<const char*>(&c.at(loader + str_off + (cn & 0xFFFFFF)))) != name)
            continue;
        cls = static_cast<u8>(cn >> 24);
        value = be32(c, s + 4);
        section = static_cast<int16_t>(be16(c, s + 8));
        return true;
    }
    return false;
}

std::vector<u8> words(std::initializer_list<u32> w)
{
    std::vector<u8> v;
    for (u32 x : w)
        for (int k = 0; k < 4; ++k) v.push_back(static_cast<u8>(x >> (24 - 8 * k)));
    return v;
}

// A driver-like container: code, data (with a TVector and pointers), imports, exports.
Container sample()
{
    Container c;
    Section code;
    code.name = "code";
    code.kind = kCode;
    code.data = words({0x7C0802A6, 0x4E800020});           // mflr r0; blr
    Section data;
    data.name = "data";
    data.kind = kUnpackedData;
    data.share = kProcessShare;
    // 0x00: TVector for DoDriverIO {code + 0, data + 0}; 0x08: pointer to an import;
    // 0x0C: pointer to another import; 0x10: a self pointer into data.
    data.data = words({0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000020, 0x11111111});
    data.total_size = 0x100;
    c.sections = {code, data};
    ImportedLibrary drv;
    drv.name = "DriverServicesLib";
    drv.symbols = {{"SynchronizeIO", kTVectorSymbol}, {"IOCommandIsComplete", kTVectorSymbol}};
    ImportedLibrary nr;
    nr.name = "NameRegistryLib";
    nr.weak = true;
    nr.symbols = {{"RegistryPropertyGet", kTVectorSymbol}};
    c.imports = {drv, nr};
    c.relocations = {
        {1, 0x00, Relocation::kSectionAddress, 0},       // TVector code word
        {1, 0x04, Relocation::kSectionAddress, 1},       // TVector TOC word = data
        {1, 0x08, Relocation::kImport, 1},               // IOCommandIsComplete
        {1, 0x0C, Relocation::kImport, 2},               // RegistryPropertyGet
        {1, 0x10, Relocation::kSectionAddress, 1},       // data + 0x20
    };
    c.exports = {{"DoDriverIO", kTVectorSymbol, 1, 0x00}, {"TheDriverDescription", kDataSymbol, 1, 0x40}};
    return c;
}

} // namespace

TEST_CASE("tools/pef/hash_functions")
{
    // Worked by hand from the published algorithm.
    CHECK_EQ(hash_word(""), 0u);
    CHECK_EQ(hash_word("a"), 0x00010061u);
    // "ab": h = ((0x61 << 1) - 0) ^ 0x62 = 0xC2 ^ 0x62 = 0xA0.
    CHECK_EQ(hash_word("ab"), 0x000200A0u);
    // "abc": h = (0xA0 << 1) ^ 0x63 = 0x140 ^ 0x63 = 0x123.
    CHECK_EQ(hash_word("abc"), 0x00030123u);
    CHECK_EQ(hash_word("DoDriverIO") >> 16, 10u);              // length in the top half
    // Longer names, where the running hash passes 16 bits and goes negative.
    // Values from a separate Python implementation written from the same
    // published algorithm (tools/pef docs): two independent codings agree.
    CHECK_EQ(hash_word("DoDriverIO"), 0x000AD1FDu);
    CHECK_EQ(hash_word("TheDriverDescription"), 0x0014BDE0u);
    CHECK_EQ(hash_word("RegistryPropertyGet"), 0x0013B3E7u);
    CHECK_EQ(hash_word("abcdefghijklmnopqrstuvwxyz"), 0x001A02D0u);
    CHECK_EQ(hash_index(0x00030123u, 0), 0u);
    CHECK_EQ(hash_index(0x00030123u, 2), (0x00030123u ^ (0x00030123u >> 2)) & 3u);
    CHECK_EQ(hash_table_power(0), 0u);
    CHECK_EQ(hash_table_power(9), 0u);
    CHECK_EQ(hash_table_power(10), 1u);
    CHECK_EQ(hash_table_power(20), 2u);
    CHECK_EQ(hash_table_power(1000), 7u);
}

TEST_CASE("tools/pef/container_layout")
{
    std::string err;
    const std::vector<u8> c = build(sample(), &err);
    REQUIRE(!c.empty());
    CHECK(err.empty());
    CHECK(std::memcmp(c.data(), "Joy!peffpwpc", 12) == 0);
    CHECK_EQ(be32(c, 12), 1u);                                // formatVersion
    CHECK_EQ(be16(c, 32), 3u);                                // code, data, loader
    CHECK_EQ(be16(c, 34), 2u);                                // instantiated
    CHECK_EQ(be32(c, 36), 0u);                                // reservedA
    // Section name table right after the headers.
    const size_t names = 40 + 3 * 28;
    CHECK(std::string(reinterpret_cast<const char*>(&c[names])) == "code");
    CHECK_EQ(be32(c, 40 + 0), 0u);                            // nameOffset of "code"
    CHECK_EQ(be32(c, 40 + 28 * 2), 0xFFFFFFFFu);              // loader: no name
    // Data section sizes: total includes the zero-initialized part.
    CHECK_EQ(be32(c, 40 + 28 + 8), 0x100u);
    CHECK_EQ(be32(c, 40 + 28 + 12), 24u);
    CHECK_EQ(c[40 + 28 + 24], kUnpackedData);
    CHECK_EQ(c[40 + 28 + 25], kProcessShare);
    // Contents alignment: code and data on 16 bytes, loader on 4.
    CHECK_EQ(be32(c, 40 + 20) % 16, 0u);
    CHECK_EQ(be32(c, 40 + 28 + 20) % 16, 0u);
    CHECK_EQ(be32(c, 40 + 56 + 20) % 4, 0u);
    CHECK_EQ(c[40 + 56 + 24], kLoader);
    // Loader header: no main/init/term, 2 libraries, 3 imports, 1 relocated section.
    const size_t ld = be32(c, 40 + 56 + 20);
    CHECK_EQ(be32(c, ld + 0), 0xFFFFFFFFu);
    CHECK_EQ(be32(c, ld + 24), 2u);
    CHECK_EQ(be32(c, ld + 28), 3u);
    CHECK_EQ(be32(c, ld + 32), 1u);
    CHECK_EQ(be32(c, ld + 44) % 4, 0u);                       // hash table 4-byte aligned
    // Second library: weak, first symbol index 2.
    const size_t lib2 = ld + 56 + 24;
    CHECK_EQ(be32(c, lib2 + 12), 1u);
    CHECK_EQ(be32(c, lib2 + 16), 2u);
    CHECK_EQ(c[lib2 + 20], 0x40u);
    // Imported symbol entries carry their class.
    CHECK_EQ(c[ld + 56 + 48], kTVectorSymbol);
}

TEST_CASE("tools/pef/prepares_at_any_address")
{
    std::string err;
    const std::vector<u8> c = build(sample(), &err);
    REQUIRE(!c.empty());
    const std::map<std::string, u32> lib = {
        {"SynchronizeIO", 0x00500000}, {"IOCommandIsComplete", 0x00500010}, {"RegistryPropertyGet", 0x00600040}};
    for (const std::vector<u32>& base : {std::vector<u32>{0x1000, 0x2000}, std::vector<u32>{0x00340000, 0x0012F000}}) {
        const Prepared p = prepare(c, base, lib);
        REQUIRE(p.ok);
        CHECK_EQ(p.word(1, 0x00), base[0]);                   // TVector: code address
        CHECK_EQ(p.word(1, 0x04), base[1]);                   // TVector: data (TOC)
        CHECK_EQ(p.word(1, 0x08), 0x00500010u);               // import #1
        CHECK_EQ(p.word(1, 0x0C), 0x00600040u);               // import #2
        CHECK_EQ(p.word(1, 0x10), base[1] + 0x20);            // added to the stored offset
        CHECK_EQ(p.word(1, 0x14), 0x11111111u);               // not relocated
        CHECK_EQ(p.mem.at(1).size(), 0x100u);
        CHECK_EQ(p.word(1, 0xFC), 0u);                        // zero-initialized tail
        CHECK_EQ(p.word(0, 0), 0x7C0802A6u);                  // code copied
    }
    // Exports through the hash table.
    u32 value = 0;
    int16_t sec = 0;
    u8 cls = 0;
    REQUIRE(find_export(c, "DoDriverIO", value, sec, cls));
    CHECK_EQ(value, 0u);
    CHECK_EQ(sec, 1);
    CHECK_EQ(cls, kTVectorSymbol);
    REQUIRE(find_export(c, "TheDriverDescription", value, sec, cls));
    CHECK_EQ(value, 0x40u);
    CHECK_EQ(cls, kDataSymbol);
    CHECK(!find_export(c, "Nope", value, sec, cls));
}

TEST_CASE("tools/pef/long_relocations_and_many_exports")
{
    // Far-apart relocations (SetPosition) and import indexes above 511 (LgByImport).
    Container c;
    Section d;
    d.kind = kUnpackedData;
    d.data.assign(0x22000, 0);
    c.sections = {d};
    ImportedLibrary lib;
    lib.name = "BigLib";
    std::map<std::string, u32> addrs;
    for (int i = 0; i < 600; ++i) {
        lib.symbols.push_back({"sym" + std::to_string(i), kDataSymbol});
        addrs["sym" + std::to_string(i)] = 0x10000000u + 16u * i;
    }
    c.imports = {lib};
    c.relocations = {{0, 0x0000, Relocation::kImport, 3},
                     {0, 0x0004, Relocation::kImport, 599},    // LgByImport
                     {0, 0x5000, Relocation::kImport, 7},      // gap > 4096: SetPosition
                     {0, 0x1000, Relocation::kSectionAddress, 0},   // out of order on input
                     {0, 0x21004, Relocation::kImport, 1}};         // offset above 64 KB
    for (int i = 0; i < 25; ++i)
        c.exports.push_back({"export" + std::to_string(i), kDataSymbol, 0, static_cast<u32>(4 * i)});
    std::string err;
    const std::vector<u8> pef = build(c, &err);
    REQUIRE(!pef.empty());
    const Prepared p = prepare(pef, {0x40000}, addrs);
    REQUIRE(p.ok);
    CHECK_EQ(p.word(0, 0x0000), 0x10000030u);
    CHECK_EQ(p.word(0, 0x0004), 0x10000000u + 16u * 599);
    CHECK_EQ(p.word(0, 0x5000), 0x10000070u);
    CHECK_EQ(p.word(0, 0x1000), 0x40000u);
    CHECK_EQ(p.word(0, 0x21004), 0x10000010u);
    CHECK_EQ(p.word(0, 0x0008), 0u);                          // untouched in between
    // 25 exports: a 4-slot table (power 2); all found.
    for (int i = 0; i < 25; ++i) {
        u32 v = 0;
        int16_t s = 0;
        u8 k = 0;
        CHECK(find_export(pef, "export" + std::to_string(i), v, s, k));
        CHECK_EQ(v, static_cast<u32>(4 * i));
    }
}

TEST_CASE("tools/pef/rejects_bad_input")
{
    std::string err;
    Container c = sample();
    c.relocations.push_back({1, 0x02, Relocation::kImport, 0});
    CHECK(build(c, &err).empty());
    CHECK(err.find("aligned") != std::string::npos);
    c = sample();
    c.relocations.push_back({1, 0x08, Relocation::kImport, 0});    // same word twice
    CHECK(build(c, &err).empty());
    c = sample();
    c.relocations.push_back({1, 0x18, Relocation::kImport, 0});    // past the initialized data
    CHECK(build(c, &err).empty());
    c = sample();
    c.relocations.push_back({1, 0x14, Relocation::kImport, 9});    // no such import
    CHECK(build(c, &err).empty());
    c = sample();
    c.sections[0].total_size = 0x100;                              // code with a zero tail
    CHECK(build(c, &err).empty());
    c = sample();
    c.exports.push_back({"x", kDataSymbol, 5, 0});
    CHECK(build(c, &err).empty());
}
