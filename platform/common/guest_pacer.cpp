// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/guest_pacer.cpp — see guest_pacer.h.

#include "platform/common/guest_pacer.h"

namespace g3 {

void GuestPacer::start(u64 host_ps, u64 guest_ps, u64 nominal_hz)
{
    host_base_ = host_ps;
    guest_base_ = guest_ps;
    nominal_ = nominal_hz ? nominal_hz : 350000000;
    hz_ = nominal_;
    win_instr_ = win_host_ = 0;
    dropped_ = 0;
}

u64 GuestPacer::plan(u64 host_ps, u64 guest_ps)
{
    const u64 target = guest_base_ + (host_ps - host_base_);
    if (guest_ps >= target)
        return 0;
    u64 lag = target - guest_ps;
    if (lag > kMaxLagPs) {
        // Too far behind to catch up smoothly: forget the excess.
        guest_base_ -= lag - kMaxLagPs;
        dropped_ += lag - kMaxLagPs;
        lag = kMaxLagPs;
    }
    return lag < kSlicePs ? lag : kSlicePs;
}

u64 GuestPacer::ran(u64 planned_ps, u64 instructions, u64 host_ps_spent)
{
    // A busy run: the instructions account for (nearly) all its guest
    // time; otherwise the guest napped and the run says nothing of speed.
    const unsigned __int128 busy_ps = static_cast<unsigned __int128>(instructions) * 1000000000000ull / hz_;
    if (planned_ps == 0 || busy_ps * 10 < static_cast<unsigned __int128>(planned_ps) * 9)
        return 0;
    win_instr_ += instructions;
    win_host_ += host_ps_spent;
    if (win_host_ < kWindowPs)
        return 0;
    // Instructions per real second while busy.
    u64 rate = static_cast<u64>(static_cast<unsigned __int128>(win_instr_) * 1000000000000ull / win_host_);
    win_instr_ = win_host_ = 0;
    if (rate < kMinHz)
        rate = kMinHz;
    if (rate > nominal_)
        rate = nominal_;
    const u64 next = (hz_ + rate) / 2;  // smoothed
    if (next == hz_)
        return 0;
    hz_ = next;
    return hz_;
}

} // namespace g3
