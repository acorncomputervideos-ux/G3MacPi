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
    void (*fn)(Ppc750& cpu, u32 insn);
    u32 insn;
};

class BlockCache {
public:
    static constexpr u32 kMaxOps = 32;
    static constexpr u32 kTableSize = 1u << 15;  // direct-mapped lookup table
    static constexpr u32 kPageShift = 12;

    struct Block {
        u32 pa;     // physical address of the first instruction
        u32 gen;    // the page generation it was built under
        u32 count;  // operations
        BlockOp ops[kMaxOps];
    };

    struct Stats {
        u64 built = 0, hits = 0, flushes = 0, invalidations = 0;
    };

    // Uses `bytes` of `arena` (8-byte aligned) for pages of RAM in
    // [0, ram_size). False if the arena is too small to be useful.
    bool init(void* arena, u32 bytes, u32 ram_size);
    bool ready() const { return blocks_ != nullptr; }

    // A valid block starting at physical address `pa`, or nullptr.
    Block* find(u32 pa)
    {
        const u32 page = pa >> kPageShift;
        if (page >= pages_)
            return nullptr;
        const u32 slot = table_[(pa >> 2) & (kTableSize - 1)];
        if (slot == 0)
            return nullptr;
        Block* b = &blocks_[slot - 1];
        if (b->pa != pa || b->gen != gen_[page])
            return nullptr;
        ++stats_.hits;
        return b;
    }
    // A fresh block for `pa` (flushing everything first if the pool is
    // full); the caller fills ops/count, then calls commit().
    Block* allocate(u32 pa);
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

    // Set when blocks were dropped; the CPU checks it after each
    // operation (the code it is running may be gone) and clears it.
    bool dirty = false;

    const Stats& stats() const { return stats_; }
    u32 capacity() const { return nblocks_; }
    u32 pages() const { return pages_; }

private:
    u32* gen_ = nullptr;    // per RAM page
    u8* code_ = nullptr;    // per RAM page: holds blocks
    u32* table_ = nullptr;  // kTableSize entries: block index + 1, 0 = empty
    Block* blocks_ = nullptr;
    u32 pages_ = 0, nblocks_ = 0, used_ = 0;
    Stats stats_;
};

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_BLOCK_CACHE_H
