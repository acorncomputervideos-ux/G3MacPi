// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/common/spsc_queue.h — lock-free single-producer / single-consumer queue.
//
// Used to pass data between an interrupt handler and the main loop on the same
// core, or between two cores (e.g. USB input -> emulator core, emulator core
// -> audio core). Exactly ONE producer may call push() and exactly ONE
// consumer may call pop(); no locks are needed under that rule.
//
// Uses GCC/Clang __atomic builtins (no <atomic>: unavailable under Circle's
// -nostdinc++). Indices are free-running u32 counters; wrap-around is safe
// because Capacity is a power of two.

#ifndef G3_CORE_COMMON_SPSC_QUEUE_H
#define G3_CORE_COMMON_SPSC_QUEUE_H

#include "core/common/types.h"

namespace g3 {

template <typename T, u32 Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscQueue capacity must be a power of two >= 2");

public:
    // Producer side. Returns false (and drops nothing) if the queue is full.
    bool push(const T& value)
    {
        const u32 head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const u32 tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        if (head - tail == Capacity)
            return false;
        items_[head & (Capacity - 1)] = value;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool pop(T& out)
    {
        const u32 tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const u32 head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (head == tail)
            return false;
        out = items_[tail & (Capacity - 1)];
        __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Approximate when called concurrently; exact when the queue is idle.
    u32 size() const
    {
        return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) -
               __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    }
    bool empty() const { return size() == 0; }
    static constexpr u32 capacity() { return Capacity; }

private:
    T items_[Capacity] = {};
    // Separate cache lines (64 bytes on Cortex-A72) to avoid false sharing
    // when producer and consumer run on different cores.
    alignas(64) u32 head_ = 0;  // written only by the producer
    alignas(64) u32 tail_ = 0;  // written only by the consumer
};

} // namespace g3

#endif // G3_CORE_COMMON_SPSC_QUEUE_H
