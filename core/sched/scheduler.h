// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/sched/scheduler.h — guest-time event scheduler.
//
// Time is a u64 count of picoseconds since machine start (2^64 ps is about
// 213 days). Devices schedule callbacks at absolute or relative times; the
// execution loop (core/machine/execution_loop.h) runs the CPU up to the next
// event, then calls advance_to(). Events at the same time fire in the order
// they were scheduled. Callbacks may schedule or cancel events, including
// ones due at the current time (they fire in the same advance_to call).
//
// Freestanding: fixed capacity, linear scan (small N; optimise if profiling
// says so).

#ifndef G3_CORE_SCHED_SCHEDULER_H
#define G3_CORE_SCHED_SCHEDULER_H

#include "core/common/types.h"

namespace g3 {

using EventCallback = void (*)(void* context, u64 now_ps);

class Scheduler {
public:
    using Handle = u32;
    static constexpr Handle kInvalidHandle = 0;
    static constexpr u32 kMaxEvents = 64;
    static constexpr u64 kNever = ~0ull;

    u64 now() const { return now_; }
    // Time as of the instruction executing now: now() plus the CPU's progress
    // in its current slice, when the execution loop supplies a clock (devices
    // that count time, such as the VIA timers, read this).
    using Clock = u64 (*)(void* context);
    void set_clock(Clock c, void* ctx) { clock_ = c; clock_ctx_ = ctx; }
    u64 precise_now() const { return clock_ ? clock_(clock_ctx_) : now_; }

    // Returns kInvalidHandle if the table is full or when is in the past.
    Handle schedule_at(u64 when_ps, EventCallback cb, void* context);
    Handle schedule_in(u64 delay_ps, EventCallback cb, void* context)
    {
        return schedule_at(now_ + delay_ps, cb, context);
    }
    // Returns false if the handle is not (or no longer) pending.
    bool cancel(Handle h);
    bool pending(Handle h) const;

    // Time of the earliest pending event, or kNever.
    u64 next_event_time() const;

    // Moves time forward to 'target' (never backwards), firing every event
    // due at or before it, in time order.
    void advance_to(u64 target_ps);

    u32 pending_count() const;

private:
    struct Slot {
        u64 when;
        u64 seq;
        EventCallback cb;
        void* ctx;
        u32 generation;
        bool active;
    };
    static Handle make_handle(u32 slot, u32 generation) { return (generation << 8) | (slot + 1); }

    Slot slots_[kMaxEvents] = {};
    u64 now_ = 0;
    u64 next_seq_ = 0;
    Clock clock_ = nullptr;
    void* clock_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_SCHED_SCHEDULER_H
