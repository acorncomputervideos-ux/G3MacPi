// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/jit_worker.h — compiling blocks on another core
// (docs/cpu/jit-a64.md, "Background compilation").
//
// With a worker attached (Ppc750::set_jit_worker), the CPU no longer
// compiles a block when it builds it: it queues a copy of the block's
// operations and carries on running them through their handlers. The
// worker, polled on another core, compiles into the JIT code buffer (which
// it then owns) and queues the result; the CPU picks results up between
// blocks and gives each block its code.
//
// Rules that keep this correct:
//  - Requests carry the block cache's flush count (the "epoch"). A result
//    is used only if the cache hasn't been flushed since: until then the
//    block is the one the request was made for. Older results are dropped.
//  - The worker empties the code buffer when it sees a request of a newer
//    epoch. The CPU makes requests only when building blocks, never while
//    running generated code, so no code still in use is overwritten.
//  - Out of room: the result says so; the CPU flushes the cache at its next
//    block build (as without a worker), which starts a new epoch.
//  - On AArch64 the CPU executes SEV after queueing a request, so a worker
//    core may wait in WFE when idle.
//  - The worker cleans the data cache and invalidates the instruction
//    cache for the code it wrote; the CPU executes an ISB before running
//    code it picked up (Arm ARM, cross-modifying code).
//
// Freestanding.

#ifndef G3_CORE_CPU_PPC750_JIT_WORKER_H
#define G3_CORE_CPU_PPC750_JIT_WORKER_H

#include "core/common/spsc_queue.h"
#include "core/cpu/ppc750/block_cache.h"
#include "core/cpu/ppc750/ppc750.h"

namespace g3::ppc {

struct JitRequest {
    BlockCache::Block* block;  // where the code goes (identifies the block)
    u64 epoch;                 // the cache's flush count when requested
    u32 pa;
    u32 gen;    // the page generation it was built under (its code depends on it)
    u32 count;
    BlockOp ops[BlockCache::kMaxOps];
};

struct JitResult {
    BlockCache::Block* block;
    u64 epoch;
    u32 pa;
    u32 code;  // word offset in the code buffer + 1; 0: out of room
    u32 native_ops, helper_ops;
};

class JitWorker {
public:
    static constexpr u32 kQueue = 256;

    // Uses the CPU's code buffer (set_jit first). The worker owns it from
    // set_jit_worker() on.
    explicit JitWorker(Ppc750& cpu) : cpu_(cpu) {}

    // Worker side (another core or thread): compiles queued blocks, at most
    // `max`. Returns how many requests it handled.
    u32 poll(u32 max = ~0u);

    struct Stats {
        u64 compiled = 0, stale = 0, full = 0, resets = 0, dropped_results = 0;
    };
    const Stats& stats() const { return stats_; }  // worker side

private:
    friend class Ppc750;
    Ppc750& cpu_;
    SpscQueue<JitRequest, kQueue> requests_;  // CPU -> worker
    SpscQueue<JitResult, kQueue> results_;    // worker -> CPU
    u64 epoch_ = 0;
    bool have_epoch_ = false;
    u32 used_ = 0;  // words of the code buffer in use this epoch
    Stats stats_;
    JitRequest req_;      // the request being compiled (not on the stack: ~0.5 KB)
    JitRequest req_out_;  // CPU side: the request being filled
    alignas(16) u8 block_[BlockCache::block_bytes(BlockCache::kMaxOps)];
};

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_JIT_WORKER_H
