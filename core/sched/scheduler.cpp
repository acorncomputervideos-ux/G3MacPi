// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/sched/scheduler.cpp — see scheduler.h.

#include "core/sched/scheduler.h"

namespace g3 {

Scheduler::Handle Scheduler::schedule_at(u64 when_ps, EventCallback cb, void* context)
{
    if (!cb || when_ps < now_)
        return kInvalidHandle;
    for (u32 i = 0; i < kMaxEvents; ++i) {
        Slot& s = slots_[i];
        if (s.active)
            continue;
        s.when = when_ps;
        s.seq = next_seq_++;
        s.cb = cb;
        s.ctx = context;
        s.generation = (s.generation + 1) & 0xFFFFFF;
        if (s.generation == 0)
            s.generation = 1;
        s.active = true;
        return make_handle(i, s.generation);
    }
    return kInvalidHandle;
}

bool Scheduler::pending(Handle h) const
{
    const u32 slot = (h & 0xFF) - 1;
    if (h == kInvalidHandle || slot >= kMaxEvents)
        return false;
    const Slot& s = slots_[slot];
    return s.active && s.generation == (h >> 8);
}

bool Scheduler::cancel(Handle h)
{
    if (!pending(h))
        return false;
    slots_[(h & 0xFF) - 1].active = false;
    return true;
}

u64 Scheduler::next_event_time() const
{
    u64 t = kNever;
    for (const Slot& s : slots_)
        if (s.active && s.when < t)
            t = s.when;
    return t;
}

u32 Scheduler::pending_count() const
{
    u32 n = 0;
    for (const Slot& s : slots_)
        n += s.active ? 1 : 0;
    return n;
}

void Scheduler::advance_to(u64 target_ps)
{
    for (;;) {
        // Earliest due event (ties: lowest sequence number = scheduled first).
        Slot* best = nullptr;
        for (Slot& s : slots_) {
            if (!s.active || s.when > target_ps)
                continue;
            if (!best || s.when < best->when || (s.when == best->when && s.seq < best->seq))
                best = &s;
        }
        if (!best)
            break;
        if (best->when > now_)
            now_ = best->when;
        best->active = false;
        best->cb(best->ctx, now_);
    }
    if (target_ps > now_)
        now_ = target_ps;
}

} // namespace g3
