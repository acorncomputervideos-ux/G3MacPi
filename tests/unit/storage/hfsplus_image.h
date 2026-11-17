// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper: builds small HFS Plus volumes from the TN1150 layout.
// (Same author as the reader: see docs/storage/hfsplus.md.)

#ifndef G3_TESTS_UNIT_STORAGE_HFSPLUS_IMAGE_H
#define G3_TESTS_UNIT_STORAGE_HFSPLUS_IMAGE_H

#include "core/common/types.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace hfs_image {

using namespace g3;

constexpr u32 kBs = 4096;     // allocation block size
constexpr u32 kNode = 4096;   // B-tree node size

struct Img {
    std::vector<u8> d;
    explicit Img(size_t n) : d(n) {}
    void u8_(size_t at, u8 v) { d.at(at) = v; }
    void u16_(size_t at, u16 v) { d.at(at) = static_cast<u8>(v >> 8); d.at(at + 1) = static_cast<u8>(v); }
    void u32_(size_t at, u32 v) { u16_(at, static_cast<u16>(v >> 16)); u16_(at + 2, static_cast<u16>(v)); }
    void u64_(size_t at, u64 v) { u32_(at, static_cast<u32>(v >> 32)); u32_(at + 4, static_cast<u32>(v)); }
};

constexpr u32 fourcc(const char* s)
{
    return (static_cast<u32>(s[0]) << 24) | (static_cast<u32>(s[1]) << 16) |
           (static_cast<u32>(s[2]) << 8) | static_cast<u32>(s[3]);
}

// One catalog record (key + data), as bytes.
struct Rec {
    u32 parent;
    std::string name;
    std::vector<u8> bytes;
};

inline std::vector<u8> key_bytes(u32 parent, const std::string& name)
{
    std::vector<u8> k(8 + 2 * name.size());
    const u16 klen = static_cast<u16>(6 + 2 * name.size());
    k[0] = static_cast<u8>(klen >> 8); k[1] = static_cast<u8>(klen);
    k[2] = static_cast<u8>(parent >> 24); k[3] = static_cast<u8>(parent >> 16);
    k[4] = static_cast<u8>(parent >> 8); k[5] = static_cast<u8>(parent);
    k[6] = static_cast<u8>(name.size() >> 8); k[7] = static_cast<u8>(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        k[8 + 2 * i] = 0;
        k[9 + 2 * i] = static_cast<u8>(name[i]);
    }
    return k;
}

inline void put_be16(std::vector<u8>& v, size_t at, u16 x) { v[at] = static_cast<u8>(x >> 8); v[at + 1] = static_cast<u8>(x); }
inline void put_be32(std::vector<u8>& v, size_t at, u32 x) { put_be16(v, at, static_cast<u16>(x >> 16)); put_be16(v, at + 2, static_cast<u16>(x)); }

inline Rec folder_rec(u32 parent, const std::string& name, u32 id)
{
    Rec r{parent, name, key_bytes(parent, name)};
    std::vector<u8> d(88);
    put_be16(d, 0, 1);       // kHFSPlusFolderRecord
    put_be32(d, 8, id);      // folderID
    r.bytes.insert(r.bytes.end(), d.begin(), d.end());
    return r;
}

struct Ext { u32 start, count; };

inline Rec file_rec(u32 parent, const std::string& name, u32 id, u32 type, u32 creator, u64 size,
             u32 total_blocks, const std::vector<Ext>& ext)
{
    Rec r{parent, name, key_bytes(parent, name)};
    std::vector<u8> d(248);
    put_be16(d, 0, 2);       // kHFSPlusFileRecord
    put_be32(d, 8, id);      // fileID
    put_be32(d, 48, type);   // userInfo.fileType
    put_be32(d, 52, creator);
    put_be32(d, 88, static_cast<u32>(size >> 32));   // dataFork.logicalSize
    put_be32(d, 92, static_cast<u32>(size));
    put_be32(d, 100, total_blocks);
    for (size_t i = 0; i < ext.size() && i < 8; ++i) {
        put_be32(d, 104 + 8 * i, ext[i].start);
        put_be32(d, 108 + 8 * i, ext[i].count);
    }
    r.bytes.insert(r.bytes.end(), d.begin(), d.end());
    return r;
}

inline Rec thread_rec(u32 cnid, u32 parent, const std::string& name, bool folder)
{
    Rec r{cnid, "", key_bytes(cnid, "")};
    std::vector<u8> d(10 + 2 * name.size());
    put_be16(d, 0, folder ? 3 : 4);
    put_be32(d, 4, parent);
    put_be16(d, 8, static_cast<u16>(name.size()));
    for (size_t i = 0; i < name.size(); ++i)
        d[11 + 2 * i] = static_cast<u8>(name[i]);
    r.bytes.insert(r.bytes.end(), d.begin(), d.end());
    return r;
}

// Writes a B-tree node with the given records into img at byte offset `at`.
inline void write_node(Img& img, size_t at, u32 flink, s8 kind, u8 height, const std::vector<std::vector<u8>>& recs)
{
    img.u32_(at + 0, flink);
    img.u32_(at + 4, 0);
    img.u8_(at + 8, static_cast<u8>(kind));
    img.u8_(at + 9, height);
    img.u16_(at + 10, static_cast<u16>(recs.size()));
    u32 off = 14;
    for (size_t i = 0; i < recs.size(); ++i) {
        img.u16_(at + kNode - 2 * (i + 1), static_cast<u16>(off));
        std::memcpy(&img.d[at + off], recs[i].data(), recs[i].size());
        off += static_cast<u32>((recs[i].size() + 1) & ~size_t(1));
    }
    img.u16_(at + kNode - 2 * (recs.size() + 1), static_cast<u16>(off));  // free space
}

inline void write_header_node(Img& img, size_t at, u16 depth, u32 root, u32 first_leaf, u32 total_nodes,
                       u16 max_key, u32 attributes)
{
    std::vector<u8> h(106);
    put_be16(h, 0, depth);
    put_be32(h, 2, root);
    put_be32(h, 10, first_leaf);
    put_be16(h, 18, kNode);
    put_be16(h, 20, max_key);
    put_be32(h, 22, total_nodes);
    h[37] = 0xCF;                     // keyCompareType: case folding
    put_be32(h, 38, attributes);
    write_node(img, at, 0, 1, 0, {h, std::vector<u8>(128), std::vector<u8>(256)});
}

inline std::vector<u8> index_rec(const Rec& first, u32 child)
{
    std::vector<u8> r = key_bytes(first.parent, first.name);
    r.resize(r.size() + 4);
    put_be32(r, r.size() - 4, child);
    return r;
}

inline std::vector<u8> fill_pattern(u32 id, size_t n)
{
    std::vector<u8> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<u8>(i * 31 + id * 7 + (i >> 12));
    return v;
}

// Builds a volume of `blocks` allocation blocks; returns the image.
struct Volume {
    Img img;
    std::vector<u8> rom, frag;
    static constexpr u64 kRomSize = 10000;
    static constexpr u64 kFragSize = 10 * kBs - 100;
    Volume() : img(64 * kBs)
    {
        Img& v = img;
        // Volume header at 1024.
        const size_t vh = 1024;
        v.u16_(vh + 0, 0x482B);   // 'H+'
        v.u16_(vh + 2, 4);
        v.u32_(vh + 40, kBs);
        v.u32_(vh + 44, 64);
        v.u32_(vh + 80, 17);      // finderInfo[0]: blessed System Folder
        v.u32_(vh + 92, 17);      // finderInfo[3]
        // Extents file: blocks 1-2, nodes 0 (header) and 1 (leaf).
        v.u64_(vh + 192, 2 * kNode);
        v.u32_(vh + 192 + 12, 2);
        v.u32_(vh + 192 + 16, 1);
        v.u32_(vh + 192 + 20, 2);
        // Catalog file: blocks 3-7, nodes 0..4.
        v.u64_(vh + 272, 5 * kNode);
        v.u32_(vh + 272 + 12, 5);
        v.u32_(vh + 272 + 16, 3);
        v.u32_(vh + 272 + 20, 5);

        // File data.
        rom = fill_pattern(32, kRomSize);
        std::memcpy(&v.d[10 * kBs], rom.data(), rom.size());          // blocks 10-12
        frag = fill_pattern(33, kFragSize);
        std::vector<Ext> fe;
        for (u32 i = 0; i < 10; ++i) {
            const u32 blk = 40 - 2 * i;                                  // scattered, descending
            fe.push_back({blk, 1});
            const size_t n = std::min<size_t>(kBs, frag.size() - i * kBs);
            std::memcpy(&v.d[blk * kBs], &frag[i * kBs], n);
        }

        // Catalog records, sorted by (parentID, name) — ASCII case-folded.
        std::vector<Rec> leafA = {
            folder_rec(1, "Macintosh HD", 2),
            thread_rec(2, 1, "Macintosh HD", true),
            folder_rec(2, "Desktop Folder", 16),
            file_rec(2, "ReadMe", 30, fourcc("TEXT"), fourcc("ttxt"), 5, 1, {{9, 1}}),
            folder_rec(2, "System Folder", 17),
        };
        std::vector<Rec> leafB = {
            thread_rec(16, 2, "Desktop Folder", true),
            thread_rec(17, 2, "System Folder", true),
            file_rec(17, "Finder", 31, fourcc("FNDR"), fourcc("MACS"), 0, 0, {}),
        };
        std::vector<Rec> leafC = {
            file_rec(17, "Fragmented", 33, fourcc("BINA"), fourcc("????"), kFragSize, 10,
                     std::vector<Ext>(fe.begin(), fe.begin() + 8)),
            file_rec(17, "Mac OS ROM", 32, fourcc("tbxi"), fourcc("chrp"), kRomSize, 3, {{10, 3}}),
        };
        const size_t cat = 3 * kBs;
        write_header_node(v, cat, 2, 1, 2, 5, 516, 6);   // big keys + variable index keys
        write_node(v, cat + 1 * kNode, 0, 0, 2,
                   {index_rec(leafA[0], 2), index_rec(leafB[0], 3), index_rec(leafC[0], 4)});
        auto bytes = [](const std::vector<Rec>& rs) {
            std::vector<std::vector<u8>> out;
            for (const Rec& r : rs) out.push_back(r.bytes);
            return out;
        };
        write_node(v, cat + 2 * kNode, 3, -1, 1, bytes(leafA));
        write_node(v, cat + 3 * kNode, 4, -1, 1, bytes(leafB));
        write_node(v, cat + 4 * kNode, 0, -1, 1, bytes(leafC));

        // Extents overflow: one leaf with the Fragmented file's last 2 extents.
        const size_t ext = 1 * kBs;
        write_header_node(v, ext, 1, 1, 1, 2, 10, 2);
        std::vector<u8> er(12 + 64);
        put_be16(er, 0, 10);       // keyLength
        er[2] = 0;                 // data fork
        put_be32(er, 4, 33);       // fileID
        put_be32(er, 8, 8);        // startBlock: after the 8 catalog extents
        put_be32(er, 12, fe[8].start); put_be32(er, 16, 1);
        put_be32(er, 20, fe[9].start); put_be32(er, 24, 1);
        write_node(v, ext + kNode, 0, -1, 1, {er});
    }
};

} // namespace hfs_image

#endif // G3_TESTS_UNIT_STORAGE_HFSPLUS_IMAGE_H
