// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/execution_loop.cpp — see execution_loop.h.

#include "core/machine/execution_loop.h"

namespace g3 {

namespace {
constexpr u64 kPsPerSecond = 1000000000000ull;
} // namespace

ExecutionLoop::ExecutionLoop(ppc::Ppc750& cpu, Scheduler& scheduler, const CpuTiming& timing)
    : cpu_(cpu), sched_(scheduler), timing_(timing)
{
    // ps per instruction = cpi * 1e12 / cpu_hz, kept with 32 fraction bits.
    ps_per_instruction_q32_ =
        (static_cast<u128>(timing_.cycles_per_instruction) * kPsPerSecond << 32) / timing_.cpu_hz;
    time_q32_ = static_cast<u128>(sched_.now()) << 32;
    run_target_ps_ = sched_.now();
    sched_.set_clock([](void* c) { return static_cast<ExecutionLoop*>(c)->precise_ps(); }, this);
    cpu_.set_time_hook(time_hook, this);
}

u64 ExecutionLoop::precise_ps() const
{
    if (!in_slice_)
        return now_ps();
    const u64 done = cpu_.instructions_executed() - slice_first_insn_;
    return static_cast<u64>((time_q32_ + ps_per_instruction_q32_ * done) >> 32);
}

void ExecutionLoop::time_hook(void* ctx)
{
    auto* self = static_cast<ExecutionLoop*>(ctx);
    self->sync_timebase(self->precise_ps());
}

u64 ExecutionLoop::instructions_until(u64 target_ps) const
{
    const u128 target_q32 = static_cast<u128>(target_ps) << 32;
    if (target_q32 <= time_q32_)
        return 0;
    // Ceiling division: the smallest n with time + n * cost >= target.
    const u128 n = (target_q32 - time_q32_ + ps_per_instruction_q32_ - 1) / ps_per_instruction_q32_;
    return n > 0xFFFFFFFFFFFFull ? 0xFFFFFFFFFFFFull : static_cast<u64>(n);
}

void ExecutionLoop::sync_timebase(u64 at_ps)
{
    // ticks = floor(t * tb_hz / 1e12), from absolute time: no drift. Never
    // backwards (a catch-up inside a slice may already have delivered them).
    const u128 due = (static_cast<u128>(at_ps) * timing_.timebase_hz) / kPsPerSecond;
    const u64 due64 = static_cast<u64>(due);
    if (due64 > tb_ticks_) {
        u64 delta = due64 - tb_ticks_;
        while (delta > 0) {  // advance_timebase takes 32-bit steps
            const u32 step = delta > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<u32>(delta);
            cpu_.advance_timebase(step);
            delta -= step;
        }
        tb_ticks_ = due64;
    }
}

void ExecutionLoop::run_for(u64 duration_ps)
{
    run_target_ps_ += duration_ps;
    const u64 end = run_target_ps_;
    while (now_ps() < end) {
        u64 target = end;
        const u64 next_event = sched_.next_event_time();
        if (next_event < target)
            target = next_event;

        // Next decrementer expiry: DEC bit 0 goes 0 -> 1 after DEC + 1 ticks.
        const u32 dec = cpu_.state().spr[ppc::spr::DEC];
        if (!(dec & 0x80000000u)) {
            const u64 tick = tb_ticks_ + static_cast<u64>(dec) + 1;
            // Earliest time at which floor(t * hz / 1e12) >= tick (ceiling).
            const u128 t = (static_cast<u128>(tick) * kPsPerSecond + timing_.timebase_hz - 1) /
                           timing_.timebase_hz;
            if (t < target)
                target = static_cast<u64>(t);
        }

        u64 n = instructions_until(target);
        if (n == 0)
            n = 1;  // target is within the current instruction: make progress
        if (n > max_slice_)
            n = max_slice_;

        if (cpu_.checkstopped()) {
            time_q32_ = static_cast<u128>(target) << 32;  // a stopped CPU just lets time pass
        } else {
            slice_first_insn_ = cpu_.instructions_executed();
            in_slice_ = true;
            cpu_.run(n);
            in_slice_ = false;
            // run() may end early (a decrementer write): count what ran.
            time_q32_ += ps_per_instruction_q32_ * (cpu_.instructions_executed() - slice_first_insn_);
        }
        sync_timebase(now_ps());
        sched_.advance_to(now_ps());
    }
}

} // namespace g3
