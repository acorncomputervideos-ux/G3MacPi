// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/hfsplus.cpp — see hfsplus.h.

#include "core/storage/hfsplus.h"

namespace g3 {

namespace {

constexpr u32 kSector = BlockDevice::kSectorSize;
constexpr u32 kExtentsFileId = 3, kCatalogFileId = 4;
constexpr s8 kLeafNode = -1, kIndexNode = 0, kHeaderNode = 1;
constexpr u16 kFolderRecord = 1, kFileRecord = 2;
constexpr u32 kVariableIndexKeys = 4;

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p)
{
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
u64 be64(const u8* p) { return (static_cast<u64>(be32(p)) << 32) | be32(p + 4); }

void parse_fork(const u8* p, u32 file_id, u8 fork_type, HfsFork& f)
{
    f.logical_size = be64(p);
    f.total_blocks = be32(p + 12);
    for (u32 i = 0; i < 8; ++i) {
        f.start[i] = be32(p + 16 + 8 * i);
        f.count[i] = be32(p + 20 + 8 * i);
    }
    f.file_id = file_id;
    f.fork_type = fork_type;
}

u16 fold_ascii(u16 c) { return (c >= 'A' && c <= 'Z') ? static_cast<u16>(c - 'A' + 'a') : c; }

bool name_matches(const HfsEntry& e, const char* name)
{
    u32 i = 0;
    for (; name[i] != '\0'; ++i) {
        if (i >= e.name_len)
            return false;
        const u16 a = e.name[i];
        const u16 b = static_cast<u8>(name[i]);
        if (a >= 0x80 || b >= 0x80) {
            if (a != b)
                return false;
        } else if (fold_ascii(a) != fold_ascii(b)) {
            return false;
        }
    }
    return i == e.name_len;
}

} // namespace

// --- raw access ------------------------------------------------------------------------

bool HfsPlusVolume::read_bytes(u64 pos, u8* buf, u32 len)
{
    while (len > 0) {
        const u64 lba = pos / kSector;
        const u32 off = static_cast<u32>(pos % kSector);
        if (off == 0 && len >= kSector) {
            const u32 n = len / kSector;
            if (!dev_->read(lba, n, buf))
                return false;
            buf += n * kSector;
            pos += static_cast<u64>(n) * kSector;
            len -= n * kSector;
            continue;
        }
        if (!dev_->read(lba, 1, sector_))
            return false;
        u32 n = kSector - off;
        if (n > len)
            n = len;
        for (u32 i = 0; i < n; ++i)
            buf[i] = sector_[off + i];
        buf += n;
        pos += n;
        len -= n;
    }
    return true;
}

// --- mounting ----------------------------------------------------------------------------

bool HfsPlusVolume::mount(BlockDevice& dev)
{
    mounted_ = false;
    dev_ = &dev;
    wrapper_offset_ = 0;
    catalog_ = Tree{};
    extents_ = Tree{};
    catalog_.node = cat_node_;
    extents_.node = ext_node_;
    u8 vh[512];
    if (!read_bytes(1024, vh, sizeof vh))
        return false;
    if (be16(vh) == 0x4244 && be16(vh + 0x7C) == 0x482B) {
        // HFS wrapper with an embedded HFS Plus volume.
        const u32 al_blk_size = be32(vh + 20);
        const u32 al_bl_st = be16(vh + 28);
        const u32 embed_start = be16(vh + 0x7E);
        wrapper_offset_ = static_cast<u64>(al_bl_st) * kSector +
                          static_cast<u64>(embed_start) * al_blk_size;
        if (!read_bytes(wrapper_offset_ + 1024, vh, sizeof vh))
            return false;
    }
    const u16 sig = be16(vh), version = be16(vh + 2);
    if (!((sig == 0x482B && version == 4) || (sig == 0x4858 && version == 5)))
        return false;
    block_size_ = be32(vh + 40);
    total_blocks_ = be32(vh + 44);
    if (block_size_ < kSector || (block_size_ & (block_size_ - 1)) != 0)
        return false;
    for (u32 i = 0; i < 8; ++i)
        finder_info_[i] = be32(vh + 80 + 4 * i);
    if (!open_tree(extents_, kExtentsFileId, vh + 192))
        return false;
    if (!open_tree(catalog_, kCatalogFileId, vh + 272))
        return false;
    mounted_ = true;
    return true;
}

bool HfsPlusVolume::open_tree(Tree& t, u32 file_id, const u8* fork_data)
{
    parse_fork(fork_data, file_id, 0, t.fork);
    if (t.fork.logical_size == 0) {
        t.node_size = 0;  // empty tree (allowed for the extents file)
        return file_id == kExtentsFileId;
    }
    u8 head[512];
    if (read(t.fork, 0, head, sizeof head) != static_cast<s64>(sizeof head))
        return false;
    if (static_cast<s8>(head[8]) != kHeaderNode)
        return false;
    const u8* h = head + 14;
    t.depth = be16(h + 0);
    t.root = be32(h + 2);
    t.first_leaf = be32(h + 10);
    t.node_size = be16(h + 18);
    t.max_key_length = be16(h + 20);
    t.variable_index_keys = (be32(h + 38) & kVariableIndexKeys) != 0;  // attributes at 38
    t.loaded = 0xFFFFFFFFu;
    return t.node_size >= 512 && t.node_size <= kMaxNodeSize &&
           (t.node_size & (t.node_size - 1)) == 0;
}

// --- B-tree nodes ----------------------------------------------------------------------

bool HfsPlusVolume::load_node(Tree& t, u32 n)
{
    if (t.node_size == 0)
        return false;
    if (t.loaded == n)
        return true;
    t.loaded = 0xFFFFFFFFu;
    const u64 off = static_cast<u64>(n) * t.node_size;
    if (read(t.fork, off, t.node, t.node_size) != static_cast<s64>(t.node_size))
        return false;
    // Sanity: the record count must fit inside the node.
    const u32 nrec = be16(t.node + 10);
    if (14 + 2 * (nrec + 1) > t.node_size)
        return false;
    t.loaded = n;
    return true;
}

u32 HfsPlusVolume::records(const Tree& t) const { return be16(t.node + 10); }

const u8* HfsPlusVolume::record(const Tree& t, u32 i) const
{
    const u32 off = be16(t.node + t.node_size - 2 * (i + 1));
    return off < t.node_size ? t.node + off : nullptr;
}

u32 HfsPlusVolume::record_limit(const Tree& t, u32 i) const
{
    // The offset list has one more entry: the start of free space.
    const u32 next = be16(t.node + t.node_size - 2 * (i + 2));
    return next <= t.node_size ? next : t.node_size;
}

// --- extents -------------------------------------------------------------------------------

bool HfsPlusVolume::map_block(const HfsFork& f, u32 fb, u32& vol_block, u32& run)
{
    u32 sum = 0;
    for (u32 i = 0; i < 8; ++i) {
        if (fb < sum + f.count[i]) {
            vol_block = f.start[i] + (fb - sum);
            run = f.count[i] - (fb - sum);
            return true;
        }
        sum += f.count[i];
    }
    if (fb >= f.total_blocks || f.file_id == kExtentsFileId)
        return false;  // the extents file itself cannot overflow
    return overflow_map(f, fb, vol_block, run);
}

bool HfsPlusVolume::overflow_map(const HfsFork& f, u32 fb, u32& vol_block, u32& run)
{
    Tree& t = extents_;
    if (t.node_size == 0)
        return false;
    // Descend: in each index node take the last record whose key is below
    // (fileID, forkType, 0); then scan the leaves forward.
    auto key_below = [&](const u8* k) {
        const u32 id = be32(k + 4);
        const u8 ft = k[2];
        if (id != f.file_id)
            return id < f.file_id;
        return ft < f.fork_type;
    };
    u32 node = t.root;
    for (u32 level = 0; level < 16; ++level) {
        if (!load_node(t, node))
            return false;
        const s8 kind = static_cast<s8>(t.node[8]);
        if (kind == kLeafNode)
            break;
        if (kind != kIndexNode)
            return false;
        u32 child = 0;
        bool have = false;
        for (u32 i = 0; i < records(t); ++i) {
            const u8* r = record(t, i);
            if (!r)
                return false;
            if (i > 0 && !key_below(r))
                break;
            const u32 klen = t.variable_index_keys ? be16(r) : t.max_key_length;
            child = be32(r + 2 + ((klen + 1) & ~1u));
            have = true;
        }
        if (!have)
            return false;
        node = child;
    }
    // Leaf scan.
    for (u32 guard = 0; guard < 1u << 20 && node != 0; ++guard) {
        if (!load_node(t, node) || static_cast<s8>(t.node[8]) != kLeafNode)
            return false;
        for (u32 i = 0; i < records(t); ++i) {
            const u8* r = record(t, i);
            if (!r)
                return false;
            const u32 id = be32(r + 4);
            const u8 ft = r[2];
            if (id > f.file_id || (id == f.file_id && ft > f.fork_type))
                return false;  // past this fork's records
            if (id != f.file_id || ft != f.fork_type)
                continue;
            u32 sum = be32(r + 8);  // startBlock of this record
            const u8* ext = r + 2 + be16(r);
            for (u32 e = 0; e < 8; ++e) {
                const u32 start = be32(ext + 8 * e), count = be32(ext + 8 * e + 4);
                if (fb >= sum && fb < sum + count) {
                    vol_block = start + (fb - sum);
                    run = count - (fb - sum);
                    return true;
                }
                sum += count;
            }
        }
        node = be32(t.node);  // fLink
    }
    return false;
}

s64 HfsPlusVolume::read(const HfsFork& f, u64 offset, u8* buf, u32 len)
{
    if (offset >= f.logical_size)
        return 0;
    if (len > f.logical_size - offset)
        len = static_cast<u32>(f.logical_size - offset);
    u32 done = 0;
    while (done < len) {
        const u64 pos = offset + done;
        const u32 fb = static_cast<u32>(pos / block_size_);
        const u32 in_block = static_cast<u32>(pos % block_size_);
        u32 vb = 0, run = 0;
        if (!map_block(f, fb, vb, run) || vb >= total_blocks_)
            return done ? static_cast<s64>(done) : -1;
        u64 avail = static_cast<u64>(run) * block_size_ - in_block;
        u32 n = len - done;
        if (avail < n)
            n = static_cast<u32>(avail);
        if (!read_bytes(wrapper_offset_ + static_cast<u64>(vb) * block_size_ + in_block, buf + done, n))
            return done ? static_cast<s64>(done) : -1;
        done += n;
    }
    return static_cast<s64>(done);
}

// --- catalog ------------------------------------------------------------------------------

bool HfsPlusVolume::catalog_leaf_for(u32 parent, u32& leaf)
{
    Tree& t = catalog_;
    u32 node = t.root;
    for (u32 level = 0; level < 16; ++level) {
        if (!load_node(t, node))
            return false;
        const s8 kind = static_cast<s8>(t.node[8]);
        if (kind == kLeafNode) {
            leaf = node;
            return true;
        }
        if (kind != kIndexNode)
            return false;
        // Last record whose parentID is below the target (or the first).
        u32 child = 0;
        bool have = false;
        for (u32 i = 0; i < records(t); ++i) {
            const u8* r = record(t, i);
            if (!r)
                return false;
            if (i > 0 && be32(r + 2) >= parent)
                break;
            const u32 klen = t.variable_index_keys ? be16(r) : t.max_key_length;
            child = be32(r + 2 + ((klen + 1) & ~1u));
            have = true;
        }
        if (!have)
            return false;
        node = child;
    }
    return false;
}

bool HfsPlusVolume::parse_entry(const u8* r, u32 limit, HfsEntry& e) const
{
    const u32 klen = be16(r);
    const u32 data = (2 + klen + 1) & ~1u;
    if (data + 2 > limit)
        return false;
    e.parent = be32(r + 2);
    e.name_len = be16(r + 6);
    if (e.name_len > 255 || 8 + 2 * e.name_len > 2 + klen)
        return false;
    for (u32 i = 0; i < e.name_len; ++i)
        e.name[i] = be16(r + 8 + 2 * i);
    const u8* d = r + data;
    const u16 type = be16(d);
    if (type == kFolderRecord) {
        if (data + 88 > limit)
            return false;
        e.folder = true;
        e.id = be32(d + 8);
        e.type = e.creator = 0;
        e.data = e.rsrc = HfsFork{};
        return true;
    }
    if (type == kFileRecord) {
        if (data + 248 > limit)
            return false;
        e.folder = false;
        e.id = be32(d + 8);
        e.type = be32(d + 48);
        e.creator = be32(d + 52);
        parse_fork(d + 88, e.id, 0x00, e.data);
        parse_fork(d + 168, e.id, 0xFF, e.rsrc);
        return true;
    }
    return false;  // thread records
}

template <typename Fn> bool HfsPlusVolume::scan_folder(u32 parent, Fn fn)
{
    if (!mounted_)
        return false;
    u32 node = 0;
    if (!catalog_leaf_for(parent, node))
        return false;
    Tree& t = catalog_;
    HfsEntry e;
    for (u32 guard = 0; guard < 1u << 20 && node != 0; ++guard) {
        if (!load_node(t, node) || static_cast<s8>(t.node[8]) != kLeafNode)
            return false;
        const u32 n = records(t);
        for (u32 i = 0; i < n; ++i) {
            const u8* r = record(t, i);
            if (!r)
                return false;
            const u32 p = be32(r + 2);
            if (p < parent)
                continue;
            if (p > parent)
                return true;  // past the folder
            const u32 limit = record_limit(t, i) - static_cast<u32>(r - t.node);
            if (!parse_entry(r, limit, e))
                continue;  // the thread record, or damage
            if (!fn(e))
                return true;
            // fn may have read other nodes (e.g. file data): reload ours.
            if (!load_node(t, node))
                return false;
        }
        node = be32(t.node);  // fLink
    }
    return true;
}

bool HfsPlusVolume::lookup(u32 parent, const char* name, HfsEntry& out)
{
    bool found = false;
    scan_folder(parent, [&](const HfsEntry& e) {
        if (name_matches(e, name)) {
            out = e;
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool HfsPlusVolume::find_by_type(u32 parent, u32 type, HfsEntry& out)
{
    bool found = false;
    scan_folder(parent, [&](const HfsEntry& e) {
        if (!e.folder && e.type == type) {
            out = e;
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool HfsPlusVolume::list(u32 parent, Visitor visit, void* context)
{
    return scan_folder(parent, [&](const HfsEntry& e) { return visit(context, e); });
}

} // namespace g3
