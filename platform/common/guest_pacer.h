// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/guest_pacer.h — keeps the Mac's time in step with real
// time.
//
// The emulated CPU's instructions advance guest time at the effective clock
// rate (ExecutionLoop::set_effective_hz). When the host emulates fewer
// instructions per second than the nominal clock (a G3 at 350 MHz), guest
// time falls behind real time: the Mac's 60 Hz timer, its cursor, clock and
// animations run in slow motion. When the guest naps (an idle Mac OS),
// guest time jumps ahead: its clock runs fast. The pacer fixes both:
//
//  - plan(): how much guest time to run now, so that guest time follows
//    real time; 0 when the guest is ahead (the host waits). A backlog over
//    kMaxLagPs is dropped rather than caught up in a burst.
//  - ran(): after each run, the measured emulation speed of busy runs (the
//    guest did not nap) sets the effective clock: the instructions the host
//    really manages per second, between kMinHz and the nominal clock. The
//    Mac then behaves like a slower G3 in real time, not a fast one in slow
//    motion.
//
// Freestanding: times in picoseconds from any monotonic host clock.

#ifndef G3_PLATFORM_COMMON_GUEST_PACER_H
#define G3_PLATFORM_COMMON_GUEST_PACER_H

#include "core/common/types.h"

namespace g3 {

class GuestPacer {
public:
    static constexpr u64 kMs = 1000000000ull;  // picoseconds
    static constexpr u64 kSlicePs = 4 * kMs;   // the most guest time one run covers
    static constexpr u64 kMaxLagPs = 250 * kMs;
    static constexpr u64 kWindowPs = 200 * kMs;  // host time measured per speed update
    static constexpr u64 kMinHz = 10000000;      // 10 MHz

    // At the start of a run of the Mac: the clocks now, and its nominal CPU clock.
    void start(u64 host_ps, u64 guest_ps, u64 nominal_hz);
    // Guest picoseconds to run now (0: the guest is ahead of real time).
    u64 plan(u64 host_ps, u64 guest_ps);
    // After running `planned` guest picoseconds: the instructions executed
    // and the host time it took. Returns a new effective clock, or 0 when
    // it stays as it is.
    u64 ran(u64 planned_ps, u64 instructions, u64 host_ps_spent);

    u64 hz() const { return hz_; }
    u64 dropped_ps() const { return dropped_; }  // backlog given up so far

private:
    u64 host_base_ = 0, guest_base_ = 0;
    u64 nominal_ = 350000000, hz_ = 350000000;
    u64 win_instr_ = 0, win_host_ = 0;
    u64 dropped_ = 0;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_GUEST_PACER_H
