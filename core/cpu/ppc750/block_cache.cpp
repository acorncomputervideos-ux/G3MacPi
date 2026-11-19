// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/block_cache.cpp — see block_cache.h.

#include "core/cpu/ppc750/block_cache.h"

namespace g3::ppc {

namespace {
template <typename T>
T* carve(u8*& p, u8* end, u32 count)
{
    const uintptr_t a = (reinterpret_cast<uintptr_t>(p) + alignof(T) - 1) & ~(uintptr_t)(alignof(T) - 1);
    u8* start = reinterpret_cast<u8*>(a);
    const u64 bytes = static_cast<u64>(count) * sizeof(T);
    if (start > end || bytes > static_cast<u64>(end - start))
        return nullptr;
    p = start + bytes;
    return reinterpret_cast<T*>(start);
}
} // namespace

bool BlockCache::init(void* arena, u32 bytes, u32 ram_size)
{
    blocks_ = nullptr;
    pages_ = (ram_size + (1u << kPageShift) - 1) >> kPageShift;
    u8* p = static_cast<u8*>(arena);
    u8* end = p + bytes;
    gen_ = carve<u32>(p, end, pages_);
    code_ = carve<u8>(p, end, pages_);
    table_ = carve<u32>(p, end, kTableSize);
    if (!gen_ || !code_ || !table_)
        return false;
    const uintptr_t a = (reinterpret_cast<uintptr_t>(p) + alignof(Block) - 1) & ~(uintptr_t)(alignof(Block) - 1);
    u8* start = reinterpret_cast<u8*>(a);
    if (start >= end)
        return false;
    nblocks_ = static_cast<u32>(static_cast<u64>(end - start) / sizeof(Block));
    if (nblocks_ < 64)
        return false;
    blocks_ = reinterpret_cast<Block*>(start);
    for (u32 i = 0; i < pages_; ++i) {
        gen_[i] = 0;
        code_[i] = 0;
    }
    flush();
    stats_ = Stats{};
    return true;
}

void BlockCache::flush()
{
    for (u32 i = 0; i < kTableSize; ++i)
        table_[i] = 0;
    for (u32 i = 0; i < pages_; ++i)
        code_[i] = 0;
    used_ = 0;
    dirty = true;
    ++stats_.flushes;
}

BlockCache::Block* BlockCache::allocate(u32 pa)
{
    if (used_ == nblocks_)
        flush();
    Block* b = &blocks_[used_++];
    b->pa = pa;
    b->count = 0;
    return b;
}

void BlockCache::commit(Block* b)
{
    const u32 page = b->pa >> kPageShift;
    b->gen = gen_[page];
    code_[page] = 1;
    table_[(b->pa >> 2) & (kTableSize - 1)] = static_cast<u32>(b - blocks_) + 1;
    ++stats_.built;
}

void BlockCache::invalidate_page(u32 page)
{
    if (page >= pages_)
        return;
    ++gen_[page];
    code_[page] = 0;
    dirty = true;
    ++stats_.invalidations;
}

} // namespace g3::ppc
