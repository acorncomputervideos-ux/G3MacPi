// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/execution_loop.h — runs the CPU against guest time.
//
// The CPU executes in slices that end at the next scheduler event, the end of
// the requested run, or the next decrementer expiry (so the decrementer
// exception is taken at the right instruction). Guest time advances by a
// fixed cost per instruction (cycles_per_instruction / cpu_hz). The time base
// is derived from ABSOLUTE guest time on every update, so it never drifts.
//
// Timing model (deliberately simple; see docs/hardware/timing.md):
//   every instruction costs 'cycles_per_instruction' CPU cycles.

#ifndef G3_CORE_MACHINE_EXECUTION_LOOP_H
#define G3_CORE_MACHINE_EXECUTION_LOOP_H

#include "core/common/types.h"
#include "core/cpu/ppc750/ppc750.h"
#include "core/sched/scheduler.h"

namespace g3 {

struct CpuTiming {
    u64 cpu_hz = 0;
    u64 timebase_hz = 0;
    u32 cycles_per_instruction = 1;
};

class ExecutionLoop {
public:
    ExecutionLoop(ppc::Ppc750& cpu, Scheduler& scheduler, const CpuTiming& timing);

    // Runs the machine for 'duration_ps' of guest time. The requested end
    // times accumulate exactly, so the up-to-one-instruction overshoot of a
    // call is absorbed by the next call instead of adding up.
    void run_for(u64 duration_ps);

    u64 now_ps() const { return static_cast<u64>(time_q32_ >> 32); }
    u64 timebase_ticks() const { return tb_ticks_; }

    // Maximum slice length in instructions (bounds interrupt latency for
    // device interrupts raised by host-side input between events).
    void set_max_slice(u64 instructions) { max_slice_ = instructions ? instructions : 1; }

private:
    using u128 = unsigned __int128;

    void sync_timebase();
    u64 instructions_until(u64 target_ps) const;

    ppc::Ppc750& cpu_;
    Scheduler& sched_;
    CpuTiming timing_;
    u128 ps_per_instruction_q32_;   // picoseconds per instruction, 32.32 fixed point
    u128 time_q32_ = 0;             // guest time, picoseconds in 32.32 fixed point
    u64 tb_ticks_ = 0;              // time-base ticks delivered so far
    u64 run_target_ps_ = 0;         // accumulated requested end time
    u64 max_slice_ = 100000;
};

} // namespace g3

#endif // G3_CORE_MACHINE_EXECUTION_LOOP_H
