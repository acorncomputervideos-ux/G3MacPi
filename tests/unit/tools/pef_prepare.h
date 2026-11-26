// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/tools/pef_prepare.h — a small PEF "preparer" for tests. It
// follows Inside Macintosh: Mac OS Runtime Architectures, chapter 8: places
// sections, executes the relocation instructions, and looks up exports
// through the hash table. Written from the spec independently of
// tools/pef/pef_writer.cpp, so that it can check the writer's output.

#ifndef G3_TESTS_UNIT_TOOLS_PEF_PREPARE_H
#define G3_TESTS_UNIT_TOOLS_PEF_PREPARE_H

#include "tools/pef/pef_writer.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pef_test {

using namespace g3::pef;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;


inline u32 be32(const std::vector<u8>& b, size_t at)
{
    return (u32(b.at(at)) << 24) | (u32(b.at(at + 1)) << 16) | (u32(b.at(at + 2)) << 8) | b.at(at + 3);
}
inline u16 be16(const std::vector<u8>& b, size_t at) { return static_cast<u16>((b.at(at) << 8) | b.at(at + 1)); }

struct Prepared {
    bool ok = false;
    std::string why;
    std::map<u32, std::vector<u8>> mem;  // section number -> contents at run time
    std::vector<u32> addr;               // section number -> run-time address
    u32 word(u32 sec, u32 off) const { return be32(mem.at(sec), off); }
};

// Prepares a container: section i is placed at base[i]; imports resolve
// through `symbols` (name -> address).
inline Prepared prepare(const std::vector<u8>& c, const std::vector<u32>& base, const std::map<std::string, u32>& symbols)
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
inline bool find_export(const std::vector<u8>& c, const std::string& name, u32& value, int16_t& section, u8& cls)
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

} // namespace pef_test

#endif // G3_TESTS_UNIT_TOOLS_PEF_PREPARE_H
