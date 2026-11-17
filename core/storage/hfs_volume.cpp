// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/hfs_volume.cpp — see hfs_volume.h.

#include "core/storage/hfs_volume.h"

namespace g3 {

namespace {

constexpr u32 kSector = BlockDevice::kSectorSize;
constexpr u32 kExtentsFileId = 3, kCatalogFileId = 4;
constexpr s8 kLeafNode = -1, kIndexNode = 0, kHeaderNode = 1;
constexpr u32 kFolderRecord = 1, kFileRecord = 2;
constexpr u32 kVariableIndexKeys = 4;

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p)
{
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
u64 be64(const u8* p) { return (static_cast<u64>(be32(p)) << 32) | be32(p + 4); }

// HFS Plus fork data (80 bytes).
void parse_fork_plus(const u8* p, u32 file_id, u8 fork_type, HfsFork& f)
{
    f = HfsFork{};
    f.logical_size = be64(p);
    f.total_blocks = be32(p + 12);
    for (u32 i = 0; i < 8; ++i) {
        f.start[i] = be32(p + 16 + 8 * i);
        f.count[i] = be32(p + 20 + 8 * i);
    }
    f.file_id = file_id;
    f.fork_type = fork_type;
}

// HFS: logical size, physical size (bytes) and a 3-extent record.
void parse_fork_classic(u32 logical, u32 physical, const u8* ext, u32 block_size, u32 file_id,
                        u8 fork_type, HfsFork& f)
{
    f = HfsFork{};
    f.logical_size = logical;
    f.total_blocks = block_size ? physical / block_size : 0;
    for (u32 i = 0; i < 3; ++i) {
        f.start[i] = be16(ext + 4 * i);
        f.count[i] = be16(ext + 4 * i + 2);
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

bool HfsVolume::read_bytes(u64 pos, u8* buf, u32 len)
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

bool HfsVolume::mount(BlockDevice& dev)
{
    mounted_ = false;
    classic_ = false;
    dev_ = &dev;
    wrapper_offset_ = 0;
    alloc_base_ = 0;
    catalog_ = Tree{};
    extents_ = Tree{};
    catalog_.node = cat_node_;
    extents_.node = ext_node_;
    u8 vh[512];
    if (!read_bytes(1024, vh, sizeof vh))
        return false;
    if (be16(vh) == 0x4244) {  // 'BD': HFS master directory block
        if (be16(vh + 0x7C) != 0x482B)
            return mounted_ = mount_classic(vh);
        // HFS wrapper with an embedded HFS Plus volume.
        const u32 al_blk_size = be32(vh + 20);
        const u32 al_bl_st = be16(vh + 28);
        const u32 embed_start = be16(vh + 0x7E);
        wrapper_offset_ = static_cast<u64>(al_bl_st) * kSector +
                          static_cast<u64>(embed_start) * al_blk_size;
        if (!read_bytes(wrapper_offset_ + 1024, vh, sizeof vh))
            return false;
    }
    return mounted_ = mount_plus(vh);
}

bool HfsVolume::mount_plus(const u8* vh)
{
    const u16 sig = be16(vh), version = be16(vh + 2);
    if (!((sig == 0x482B && version == 4) || (sig == 0x4858 && version == 5)))
        return false;
    block_size_ = be32(vh + 40);
    total_blocks_ = be32(vh + 44);
    alloc_base_ = wrapper_offset_;
    if (block_size_ < kSector || (block_size_ & (block_size_ - 1)) != 0)
        return false;
    for (u32 i = 0; i < 8; ++i)
        finder_info_[i] = be32(vh + 80 + 4 * i);
    parse_fork_plus(vh + 192, kExtentsFileId, 0, extents_.fork);
    parse_fork_plus(vh + 272, kCatalogFileId, 0, catalog_.fork);
    return open_tree(extents_) && open_tree(catalog_);
}

bool HfsVolume::mount_classic(const u8* mdb)
{
    classic_ = true;
    total_blocks_ = be16(mdb + 18);
    block_size_ = be32(mdb + 20);
    alloc_base_ = static_cast<u64>(be16(mdb + 28)) * kSector;
    // HFS allocation blocks are multiples of 512 bytes (not necessarily powers of two).
    if (block_size_ < kSector || block_size_ % kSector != 0)
        return false;
    for (u32 i = 0; i < 8; ++i)
        finder_info_[i] = be32(mdb + 92 + 4 * i);
    const u32 xt_size = be32(mdb + 130), ct_size = be32(mdb + 146);
    parse_fork_classic(xt_size, xt_size, mdb + 134, block_size_, kExtentsFileId, 0, extents_.fork);
    parse_fork_classic(ct_size, ct_size, mdb + 150, block_size_, kCatalogFileId, 0, catalog_.fork);
    return open_tree(extents_) && open_tree(catalog_);
}

bool HfsVolume::open_tree(Tree& t)
{
    if (t.fork.logical_size == 0) {
        t.node_size = 0;  // empty tree: allowed for the extents file only
        return t.fork.file_id == kExtentsFileId;
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

bool HfsVolume::load_node(Tree& t, u32 n)
{
    if (t.node_size == 0)
        return false;
    if (t.loaded == n)
        return true;
    t.loaded = 0xFFFFFFFFu;
    const u64 off = static_cast<u64>(n) * t.node_size;
    if (read(t.fork, off, t.node, t.node_size) != static_cast<s64>(t.node_size))
        return false;
    const u32 nrec = be16(t.node + 10);
    if (14 + 2 * (nrec + 1) > t.node_size)
        return false;
    t.loaded = n;
    return true;
}

u32 HfsVolume::records(const Tree& t) const { return be16(t.node + 10); }

const u8* HfsVolume::record(const Tree& t, u32 i) const
{
    const u32 off = be16(t.node + t.node_size - 2 * (i + 1));
    return off < t.node_size ? t.node + off : nullptr;
}

u32 HfsVolume::record_limit(const Tree& t, u32 i) const
{
    const u32 next = be16(t.node + t.node_size - 2 * (i + 2));  // start of free space for the last
    return next <= t.node_size ? next : t.node_size;
}

u32 HfsVolume::key_size(const Tree& t, const u8* r, bool index) const
{
    // HFS keys have a one-byte length, HFS Plus keys a two-byte one. Index
    // keys have a fixed size unless the tree uses variable index keys.
    const u32 len_field = classic_ ? 1 : 2;
    const u32 len = (index && !t.variable_index_keys) ? t.max_key_length
                                                       : (classic_ ? r[0] : be16(r));
    return (len_field + len + 1) & ~1u;  // data is 2-byte aligned
}

// --- extents -------------------------------------------------------------------------------

bool HfsVolume::map_block(const HfsFork& f, u32 fb, u32& vol_block, u32& run)
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

bool HfsVolume::overflow_map(const HfsFork& f, u32 fb, u32& vol_block, u32& run)
{
    Tree& t = extents_;
    if (t.node_size == 0)
        return false;
    // Key fields: HFS {len u8, forkType @1, fileID @2, start u16 @6};
    // HFS Plus {len u16, forkType @2, pad, fileID @4, start u32 @8}.
    auto key_id = [&](const u8* k) { return be32(k + (classic_ ? 2 : 4)); };
    auto key_fork = [&](const u8* k) { return k[classic_ ? 1 : 2]; };
    auto key_start = [&](const u8* k) { return classic_ ? be16(k + 6) : be32(k + 8); };
    auto key_below = [&](const u8* k) {
        if (key_id(k) != f.file_id)
            return key_id(k) < f.file_id;
        return key_fork(k) < f.fork_type;
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
            child = be32(r + key_size(t, r, true));
            have = true;
        }
        if (!have)
            return false;
        node = child;
    }
    const u32 n_ext = classic_ ? 3 : 8;
    for (u32 guard = 0; guard < 1u << 20 && node != 0; ++guard) {
        if (!load_node(t, node) || static_cast<s8>(t.node[8]) != kLeafNode)
            return false;
        for (u32 i = 0; i < records(t); ++i) {
            const u8* r = record(t, i);
            if (!r)
                return false;
            if (key_id(r) > f.file_id || (key_id(r) == f.file_id && key_fork(r) > f.fork_type))
                return false;  // past this fork's records
            if (key_id(r) != f.file_id || key_fork(r) != f.fork_type)
                continue;
            u32 sum = key_start(r);
            const u8* ext = r + key_size(t, r, false);
            for (u32 e = 0; e < n_ext; ++e) {
                const u32 start = classic_ ? be16(ext + 4 * e) : be32(ext + 8 * e);
                const u32 count = classic_ ? be16(ext + 4 * e + 2) : be32(ext + 8 * e + 4);
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

s64 HfsVolume::read(const HfsFork& f, u64 offset, u8* buf, u32 len)
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
        const u64 avail = static_cast<u64>(run) * block_size_ - in_block;
        u32 n = len - done;
        if (avail < n)
            n = static_cast<u32>(avail);
        if (!read_bytes(alloc_base_ + static_cast<u64>(vb) * block_size_ + in_block, buf + done, n))
            return done ? static_cast<s64>(done) : -1;
        done += n;
    }
    return static_cast<s64>(done);
}

// --- catalog ------------------------------------------------------------------------------

bool HfsVolume::catalog_leaf_for(u32 parent, u32& leaf)
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
        // parentID is at key offset 2 in both formats.
        u32 child = 0;
        bool have = false;
        for (u32 i = 0; i < records(t); ++i) {
            const u8* r = record(t, i);
            if (!r)
                return false;
            if (i > 0 && be32(r + 2) >= parent)
                break;
            child = be32(r + key_size(t, r, true));
            have = true;
        }
        if (!have)
            return false;
        node = child;
    }
    return false;
}

bool HfsVolume::parse_entry(const u8* r, u32 limit, HfsEntry& e) const
{
    const u32 data = key_size(catalog_, r, false);
    if (data + 2 > limit)
        return false;
    e.parent = be32(r + 2);
    const u8* d = r + data;
    if (classic_) {
        const u32 klen = r[0];
        e.name_len = r[6];
        if (e.name_len > 31 || 6 + e.name_len > klen)
            return false;
        for (u32 i = 0; i < e.name_len; ++i)
            e.name[i] = r[7 + i];
        const u32 type = d[0];
        if (type == kFolderRecord) {
            if (data + 70 > limit)
                return false;
            e.folder = true;
            e.id = be32(d + 6);
            e.type = e.creator = 0;
            e.data = e.rsrc = HfsFork{};
            return true;
        }
        if (type == kFileRecord) {
            if (data + 102 > limit)
                return false;
            e.folder = false;
            e.type = be32(d + 4);
            e.creator = be32(d + 8);
            e.id = be32(d + 20);
            parse_fork_classic(be32(d + 26), be32(d + 30), d + 74, block_size_, e.id, 0x00, e.data);
            parse_fork_classic(be32(d + 36), be32(d + 40), d + 86, block_size_, e.id, 0xFF, e.rsrc);
            return true;
        }
        return false;  // thread records
    }
    const u32 klen = be16(r);
    e.name_len = be16(r + 6);
    if (e.name_len > 255 || 8 + 2 * e.name_len > 2 + klen)
        return false;
    for (u32 i = 0; i < e.name_len; ++i)
        e.name[i] = be16(r + 8 + 2 * i);
    const u32 type = be16(d);
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
        parse_fork_plus(d + 88, e.id, 0x00, e.data);
        parse_fork_plus(d + 168, e.id, 0xFF, e.rsrc);
        return true;
    }
    return false;  // thread records
}

template <typename Fn> bool HfsVolume::scan_folder(u32 parent, Fn fn)
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
            if (!load_node(t, node))  // fn may have loaded other nodes
                return false;
        }
        node = be32(t.node);  // fLink
    }
    return true;
}

bool HfsVolume::lookup(u32 parent, const char* name, HfsEntry& out)
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

bool HfsVolume::find_by_type(u32 parent, u32 type, HfsEntry& out)
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

bool HfsVolume::list(u32 parent, Visitor visit, void* context)
{
    return scan_folder(parent, [&](const HfsEntry& e) { return visit(context, e); });
}

} // namespace g3
