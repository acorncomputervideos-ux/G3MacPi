// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/disk_format.cpp — see disk_format.h.

#include "core/storage/disk_format.h"

#include "core/common/endian.h"

namespace g3 {

namespace {

constexpr u32 kSector = 512;
constexpr u32 kNodeSize = 512;         // HFS B-tree nodes
constexpr u32 kCatalogMaxKey = 37;     // Inside Macintosh: Files (catalog)
constexpr u32 kExtentsMaxKey = 7;      // Inside Macintosh: Files (extents)
constexpr u32 kFirstUserCnid = 16;     // CNIDs below 16 are reserved
constexpr u16 kAttrUnmounted = 0x0100; // drAtrb: volume was cleanly unmounted

void fill(u8* p, u64 n, u8 v)
{
    for (u64 i = 0; i < n; ++i)
        p[i] = v;
}

void put16(u8* p, u32 v) { store_be16(p, static_cast<u16>(v)); }
void put32(u8* p, u32 v) { store_be32(p, v); }

u32 name_length(const char* s)
{
    u32 n = 0;
    while (s[n])
        ++n;
    return n;
}

bool valid_name(const char* s, u32 max)
{
    const u32 n = name_length(s);
    if (n == 0 || n > max)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (s[i] == ':')
            return false;
    return true;
}

// Pascal string into a field of `field` bytes (length byte + characters).
void put_pstring(u8* p, const char* s, u32 field)
{
    fill(p, field, 0);
    const u32 n = name_length(s);
    p[0] = static_cast<u8>(n);
    for (u32 i = 0; i < n && i + 1 < field; ++i)
        p[1 + i] = static_cast<u8>(s[i]);
}

// B-tree node helpers (512-byte nodes).
void node_descriptor(u8* node, s8 kind, u8 height, u16 records)
{
    put32(node + 0, 0);  // fLink
    put32(node + 4, 0);  // bLink
    node[8] = static_cast<u8>(kind);
    node[9] = height;
    put16(node + 10, records);
    put16(node + 12, 0);
}
// Offset of record i (and, for i = numRecords, of the free space).
void set_offset(u8* node, u32 i, u32 off) { put16(node + kNodeSize - 2 * (i + 1), off); }

// Header node (node 0) of a B-tree file of `nodes` nodes.
void header_node(u8* node, u32 depth, u32 root, u32 leaf_records, u32 first_leaf, u32 last_leaf,
                 u32 max_key, u32 nodes, u32 used_nodes)
{
    fill(node, kNodeSize, 0);
    node_descriptor(node, 1, 0, 3);
    u8* h = node + 14;  // header record, 106 bytes
    put16(h + 0, depth);
    put32(h + 2, root);
    put32(h + 6, leaf_records);
    put32(h + 10, first_leaf);
    put32(h + 14, last_leaf);
    put16(h + 18, kNodeSize);
    put16(h + 20, max_key);
    put32(h + 22, nodes);
    put32(h + 26, nodes - used_nodes);
    // 128-byte user data record at 120, map record at 248 up to the offsets.
    u8* map = node + 248;
    for (u32 n = 0; n < used_nodes; ++n)
        map[n / 8] |= static_cast<u8>(0x80u >> (n % 8));
    set_offset(node, 0, 14);
    set_offset(node, 1, 120);
    set_offset(node, 2, 248);
    set_offset(node, 3, kNodeSize - 8);  // free space: right after the map record
}

// Catalog key in a leaf node; returns its size padded to an even length.
u32 catalog_key(u8* p, u32 parent, const char* name)
{
    const u32 n = name_length(name);
    p[0] = static_cast<u8>(6 + n);  // keyLength: reserved + parentID + Str31 length + chars
    p[1] = 0;
    put32(p + 2, parent);
    p[6] = static_cast<u8>(n);
    for (u32 i = 0; i < n; ++i)
        p[7 + i] = static_cast<u8>(name[i]);
    u32 size = 7 + n;
    if (size & 1)
        p[size++] = 0;
    return size;
}

} // namespace

bool format_hfs(u8* vol, u64 bytes, const HfsFormatOptions& opt)
{
    if (bytes % kSector || bytes < 800 * 1024 || bytes > 0xFFFFFFFFull * kSector)
        return false;
    if (!opt.volume_name || !valid_name(opt.volume_name, 27))
        return false;
    const u64 sectors = bytes / kSector;

    // Allocation block size: the smallest multiple of 512 bytes that keeps
    // the block count within 16 bits.
    const u64 usable = sectors - 3 - 2;  // boot blocks + MDB, alternate MDB + reserved
    u32 al_blk = kSector;
    while (usable * kSector / al_blk > 0xFFFF)
        al_blk += kSector;
    // Bitmap blocks, then recompute the allocation block count from the rest.
    u32 nm_al_blks = static_cast<u32>(usable * kSector / al_blk);
    const u32 bitmap_sectors = (nm_al_blks + 4095) / 4096;
    const u32 al_bl_st = 3 + bitmap_sectors;
    nm_al_blks = static_cast<u32>((sectors - al_bl_st - 2) * kSector / al_blk);
    if (nm_al_blks > 0xFFFF)
        nm_al_blks = 0xFFFF;

    // B-tree files: about 1/256 of the volume each, at least 16 KB, at most
    // 4 MB, in whole allocation blocks.
    u64 tree_bytes = bytes / 256;
    if (tree_bytes < 16 * 1024) tree_bytes = 16 * 1024;
    if (tree_bytes > 4 * 1024 * 1024) tree_bytes = 4 * 1024 * 1024;
    const u32 tree_blocks = static_cast<u32>((tree_bytes + al_blk - 1) / al_blk);
    const u32 tree_size = tree_blocks * al_blk;
    const u32 tree_nodes = tree_size / kNodeSize;
    if (2 * tree_blocks >= nm_al_blks)
        return false;

    fill(vol, bytes, 0);
    auto alloc = [&](u32 block) { return vol + static_cast<u64>(al_bl_st) * kSector + static_cast<u64>(block) * al_blk; };

    // Extents overflow file: allocation blocks [0, tree_blocks); empty tree.
    header_node(alloc(0), 0, 0, 0, 0, 0, kExtentsMaxKey, tree_nodes, 1);

    // Catalog file: allocation blocks [tree_blocks, 2 * tree_blocks).
    u8* cat = alloc(tree_blocks);
    header_node(cat, 1, 1, 2, 1, 1, kCatalogMaxKey, tree_nodes, 2);
    u8* leaf = cat + kNodeSize;
    node_descriptor(leaf, -1, 1, 2);
    u32 pos = 14;
    // Record 0: the root folder, key (parent 1, volume name).
    set_offset(leaf, 0, pos);
    pos += catalog_key(leaf + pos, 1, opt.volume_name);
    u8* dir = leaf + pos;
    dir[0] = 1;                              // cdrType: folder
    put16(dir + 4, 0);                       // dirVal: no items
    put32(dir + 6, 2);                       // dirDirID: the root folder
    put32(dir + 10, opt.date);               // dirCrDat
    put32(dir + 14, opt.date);               // dirMdDat
    pos += 70;
    // Record 1: the root's thread, key (parent 2, empty name).
    set_offset(leaf, 1, pos);
    pos += catalog_key(leaf + pos, 2, "");
    u8* thd = leaf + pos;
    thd[0] = 3;                              // cdrType: folder thread
    put32(thd + 10, 1);                      // thdParID
    put_pstring(thd + 14, opt.volume_name, 32);  // thdCName
    pos += 46;
    set_offset(leaf, 2, pos);                // free space

    // Volume bitmap: the two B-tree files.
    u8* bitmap = vol + 3 * kSector;
    for (u32 b = 0; b < 2 * tree_blocks; ++b)
        bitmap[b / 8] |= static_cast<u8>(0x80u >> (b % 8));

    // Master Directory Block, and its copy.
    u8* mdb = vol + 2 * kSector;
    put16(mdb + 0, 0x4244);                  // 'BD'
    put32(mdb + 2, opt.date);
    put32(mdb + 6, opt.date);
    put16(mdb + 10, kAttrUnmounted);
    put16(mdb + 14, 3);                      // drVBMSt
    put16(mdb + 16, 2 * tree_blocks);        // drAllocPtr
    put16(mdb + 18, nm_al_blks);
    put32(mdb + 20, al_blk);
    put32(mdb + 24, 4 * al_blk);             // drClpSiz
    put16(mdb + 28, al_bl_st);
    put32(mdb + 30, kFirstUserCnid);         // drNxtCNID
    put16(mdb + 34, nm_al_blks - 2 * tree_blocks);  // drFreeBks
    put_pstring(mdb + 36, opt.volume_name, 28);
    put32(mdb + 74, tree_size);              // drXTClpSiz
    put32(mdb + 78, tree_size);              // drCTClpSiz
    put32(mdb + 130, tree_size);             // drXTFlSize
    put16(mdb + 134, 0);                     // drXTExtRec[0]
    put16(mdb + 136, tree_blocks);
    put32(mdb + 146, tree_size);             // drCTFlSize
    put16(mdb + 150, tree_blocks);           // drCTExtRec[0]
    put16(mdb + 152, tree_blocks);
    u8* alt = vol + (sectors - 2) * kSector;
    for (u32 i = 0; i < kSector; ++i)
        alt[i] = mdb[i];
    return true;
}

bool format_apm_hfs(u8* disk, u64 bytes, const HfsFormatOptions& opt)
{
    if (bytes % kSector || bytes / kSector <= kApmHfsStart || bytes / kSector > 0xFFFFFFFFull)
        return false;
    const u32 sectors = static_cast<u32>(bytes / kSector);
    const u32 hfs_blocks = sectors - kApmHfsStart;
    if (!format_hfs(disk + static_cast<u64>(kApmHfsStart) * kSector,
                    static_cast<u64>(hfs_blocks) * kSector, opt))
        return false;
    fill(disk, static_cast<u64>(kApmHfsStart) * kSector, 0);

    // Driver descriptor map: no drivers.
    put16(disk + 0, 0x4552);  // 'ER'
    put16(disk + 2, kSector);
    put32(disk + 4, sectors);

    struct Entry { u32 start, count; const char* name; const char* type; u32 status; };
    // Status bits (Inside Macintosh: Devices): valid 1, allocated 2, in use 4,
    // readable 0x10, writable 0x20.
    const Entry entries[2] = {
        {1, kApmHfsStart - 1, "Apple", "Apple_partition_map", 0x03},
        {kApmHfsStart, hfs_blocks, opt.volume_name, "Apple_HFS", 0x37},
    };
    for (u32 i = 0; i < 2; ++i) {
        u8* pm = disk + (1 + i) * kSector;
        put16(pm + 0, 0x504D);  // 'PM'
        put32(pm + 4, 2);       // pmMapBlkCnt: entries in the map
        put32(pm + 8, entries[i].start);
        put32(pm + 12, entries[i].count);
        for (u32 k = 0; entries[i].name[k] && k < 31; ++k)
            pm[16 + k] = static_cast<u8>(entries[i].name[k]);
        for (u32 k = 0; entries[i].type[k] && k < 31; ++k)
            pm[48 + k] = static_cast<u8>(entries[i].type[k]);
        put32(pm + 80, 0);                 // pmLgDataStart
        put32(pm + 84, entries[i].count);  // pmDataCnt
        put32(pm + 88, entries[i].status);
    }
    return true;
}

} // namespace g3
