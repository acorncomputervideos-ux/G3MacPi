// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/block_cache.h — decoded-instruction blocks for the 750
// (JIT stage 1, docs/cpu/block-cache.md).
//
// A block is the straight-line code starting at one physical address, up
// to its first branch or other block-ending instruction, the end of its
// 4 KB page, or kMaxOps instructions. Each instruction is decoded once into
// an operation (handler + instruction word); later visits replay the
// operations without fetching, translating or decoding again.
//
// Blocks are found by physical address. Code changes are caught per page:
// every RAM page has a generation number, and a block is valid while its
// page's generation is the one it was built under. Writing to a page that
// holds code (CPU stores, writes through the bus, DMA) or icbi on it bumps
// the generation, which drops all of the page's blocks at once.
//
// The cache works in caller-supplied memory (no allocation): when the
// block pool is full, everything is flushed and refilled.

#ifndef G3_CORE_CPU_PPC750_BLOCK_CACHE_H
#define G3_CORE_CPU_PPC750_BLOCK_CACHE_H

#include "core/common/types.h"

namespace g3::ppc {

class Ppc750;

struct BlockOp {
    using Fn = void (*)(Ppc750& cpu, u32 insn);
    Fn fn;
    u32 insn;
};

class BlockCache {
public:
    static constexpr u32 kMaxOps = 32;
    static constexpr u32 kTableSize = 1u << 15;  // direct-mapped lookup table
    static constexpr u32 kPageShift = 12;

    // Blocks are packed one after another in the pool, each with exactly
    // its own operations (most blocks are a few instructions long).
    struct Block {
        u32 pa;     // physical address of the first instruction
        u32 gen;    // the page generation it was built under
        u32 count;  // operations
        u32 code;   // generated code: word offset in the JIT buffer + 1; 0 = none
        BlockOp ops[1];  // really `count`
    };
    static constexpr u32 block_bytes(u32 ops)
    {
        return static_cast<u32>(sizeof(Block) + (ops - 1) * sizeof(BlockOp));
    }

    struct Stats {
        u64 built = 0, hits = 0, flushes = 0, invalidations = 0;
    };

    // Uses `bytes` of `arena` (8-byte aligned) for pages of RAM in
    // [0, ram_size). False if the arena is too small to be useful.
    bool init(void* arena, u32 bytes, u32 ram_size);
    bool ready() const { return pool_ != nullptr; }

    // A valid block starting at physical address `pa`, or nullptr.
    Block* find(u32 pa)
    {
        const u32 page = pa >> kPageShift;
        if (page >= pages_)
            return nullptr;
        const u32 slot = table_[(pa >> 2) & (kTableSize - 1)];
        if (slot == 0)
            return nullptr;
        Block* b = reinterpret_cast<Block*>(pool_ + (slot - 1) * kAlign);
        if (b->pa != pa || b->gen != gen_[page])
            return nullptr;
        ++stats_.hits;
        return b;
    }
    // A fresh block for `pa` with room for `ops` operations (flushing
    // everything first if the pool is full); the caller fills ops and
    // count (at most `ops`), then calls commit().
    Block* allocate(u32 pa, u32 ops);
    void commit(Block* b);

    // RAM at [pa, pa + len) was written: drops the blocks of the pages it
    // touches if they hold code. Cheap when they do not.
    void note_write(u32 pa, u32 len)
    {
        const u32 first = pa >> kPageShift;
        const u32 last = (pa + (len ? len - 1 : 0)) >> kPageShift;
        for (u32 p = first; p <= last && p < pages_; ++p)
            if (code_[p])
                invalidate_page(p);
    }
    void invalidate_page(u32 page);
    void flush();

    // Set to 1 when blocks are dropped (the code the CPU is running may
    // be gone); the CPU points it at its block-exit flag.
    void set_signal(u8* flag) { signal_ = flag; }

    const Stats& stats() const { return stats_; }
    // Where the lookup's data lives, for generated code that finds the
    // next block itself (ppc750_jit.cpp): fixed after init().
    struct Layout {
        const u32* table;
        const u8* pool;
        const u32* gen;
        u32 pages;
    };
    Layout layout() const { return {table_, pool_, gen_, pages_}; }
    static constexpr u32 kAlign = 16;  // a block's pool offset is a multiple of this
    // Blocks of kMaxOps operations that fit (more, shorter ones do).
    u32 capacity() const { return pool_bytes_ / block_bytes(kMaxOps); }
    u32 pages() const { return pages_; }
    // Per RAM page: nonzero if blocks were built from it (for the JIT's
    // inline stores, which must leave such pages to note_write()).
    const u8* code_pages() const { return code_; }

private:
    u32* gen_ = nullptr;    // per RAM page
    u8* code_ = nullptr;    // per RAM page: holds blocks
    u32* table_ = nullptr;  // kTableSize entries: pool offset / kAlign + 1, 0 = empty
    u8* pool_ = nullptr;
    u32 pages_ = 0, pool_bytes_ = 0, used_ = 0;  // used_: bytes
    u8 own_signal_ = 0;
    u8* signal_ = &own_signal_;
    Stats stats_;
};

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_BLOCK_CACHE_H
