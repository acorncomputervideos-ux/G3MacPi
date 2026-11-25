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
    pool_ = nullptr;
    pages_ = (ram_size + (1u << kPageShift) - 1) >> kPageShift;
    u8* p = static_cast<u8*>(arena);
    u8* end = p + bytes;
    gen_ = carve<u32>(p, end, pages_);
    code_ = carve<u8>(p, end, pages_);
    u32 bits = kMinTableBits;
    while (bits < kMaxTableBits && (4ull << (bits + 1)) <= bytes / 8)
        ++bits;
    table_mask_ = (1u << bits) - 1;
    table_ = carve<u32>(p, end, table_mask_ + 1);
    if (!gen_ || !code_ || !table_)
        return false;
    const uintptr_t a = (reinterpret_cast<uintptr_t>(p) + kAlign - 1) & ~static_cast<uintptr_t>(kAlign - 1);
    u8* start = reinterpret_cast<u8*>(a);
    if (start >= end)
        return false;
    pool_bytes_ = static_cast<u32>(end - start) & ~(kAlign - 1);
    if (pool_bytes_ < 64 * block_bytes(kMaxOps))
        return false;
    pool_ = start;
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
    for (u32 i = 0; i <= table_mask_; ++i)
        table_[i] = 0;
    for (u32 i = 0; i < pages_; ++i)
        code_[i] = 0;
    used_ = 0;
    *signal_ = 1;
    ++stats_.flushes;
    if (on_flushed_)
        on_flushed_(hook_ctx_);
}

BlockCache::Block* BlockCache::allocate(u32 pa, u32 ops)
{
    const u32 need = (block_bytes(ops) + kAlign - 1) & ~(kAlign - 1);
    if (pool_bytes_ - used_ < need)
        flush();
    Block* b = reinterpret_cast<Block*>(pool_ + used_);
    used_ += need;
    b->pa = pa;
    b->count = 0;
    b->code = 0;
    b->asked = 0;
    return b;
}

void BlockCache::commit(Block* b)
{
    const u32 page = b->pa >> kPageShift;
    b->gen = gen_[page];
    code_[page] = 1;
    table_[(b->pa >> 2) & table_mask_] =
        static_cast<u32>((reinterpret_cast<u8*>(b) - pool_) / kAlign) + 1;
    ++stats_.built;
}

void BlockCache::invalidate_page(u32 page)
{
    if (page >= pages_)
        return;
    ++gen_[page];
    code_[page] = 0;
    *signal_ = 1;
    ++stats_.invalidations;
    if (on_invalidated_)
        on_invalidated_(hook_ctx_, page);
}

} // namespace g3::ppc
