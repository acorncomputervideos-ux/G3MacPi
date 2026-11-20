// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the PEF writer. The preparer in pef_prepare.h follows Inside
// Macintosh's description of how the Code Fragment Manager places sections
// and executes relocation instructions, and looks up exports through the
// hash table; the writer's output must prepare correctly at any addresses.

#include "tests/framework/g3test.h"
#include "tests/unit/tools/pef_prepare.h"
#include "tools/pef/pef_writer.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace g3::pef;
using namespace pef_test;

namespace {

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
    c.sections[0].kind = kExecutableData;                          // writable code may have one
    CHECK(!build(c, &err).empty());
    c = sample();
    c.exports.push_back({"x", kDataSymbol, 5, 0});
    CHECK(build(c, &err).empty());
}
