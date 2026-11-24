// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_guest_pacer.cpp — guest time kept in step with
// real time: a slow host lowers the effective clock until the Mac's time
// follows real time; an idle (napping) guest does not run ahead.

#include "platform/common/guest_pacer.h"
#include "tests/framework/g3test.h"

#include <cstdint>

using namespace g3;

namespace {
constexpr u64 kMs = GuestPacer::kMs;
constexpr u64 kNominal = 350000000;

// A simulated host and guest: the host emulates `mips` million instructions
// per second; the guest is busy for `busy` of each run (else it naps).
struct Sim {
    GuestPacer p;
    u64 host = 0, guest = 0, hz = kNominal, instr = 0;
    double mips;
    double busy;
    Sim(double m, double b) : mips(m), busy(b) { p.start(host, guest, kNominal); }
    u64 steps = 0;
    bool ok() const { return steps < 20000000; }  // a pacer that never waits must not hang the test
    void step()
    {
        ++steps;
        const u64 run = p.plan(host, guest);
        if (run == 0) {
            host += kMs / 2;  // ahead of real time: the host waits a little
            return;
        }
        // The busy part executes instructions at the effective clock's
        // rate of guest time; the rest is a nap (no host time).
        const u64 busy_ps = static_cast<u64>(run * busy);
        const u64 n = static_cast<u64>(static_cast<unsigned __int128>(busy_ps) * hz / 1000000000000ull);
        const u64 spent = static_cast<u64>(n / mips * 1e6);  // ps of host time
        guest += run;
        host += spent;
        instr += n;
        const u64 next = p.ran(run, n, spent);
        if (next)
            hz = next;
    }
};
} // namespace

TEST_CASE("platform/pacer/slow_host_lowers_the_clock_to_real_time")
{
    Sim s(40.0, 1.0);  // 40 MIPS, always busy
    while (s.host < 10000 * kMs)
        s.step();
    // The effective clock settles at the host's speed (within 5%).
    CHECK(s.hz > 38000000);
    CHECK(s.hz < 42000000);
    // In the last seconds guest time advances with real time.
    const u64 h0 = s.host, g0 = s.guest;
    while (s.host < h0 + 5000 * kMs)
        s.step();
    const double ratio = static_cast<double>(s.guest - g0) / static_cast<double>(s.host - h0);
    CHECK(ratio > 0.97);
    CHECK(ratio < 1.03);
    // Never more than the allowed lag behind real time (once the backlog
    // the slow start built up is given up), never ahead.
    CHECK(s.guest <= s.host);
    CHECK(s.host - s.guest - s.p.dropped_ps() <= GuestPacer::kMaxLagPs + GuestPacer::kSlicePs);
}

TEST_CASE("platform/pacer/fast_host_keeps_the_nominal_clock_and_waits")
{
    Sim s(2000.0, 1.0);  // faster than a 350 MHz G3
    while (s.host < 3000 * kMs)
        s.step();
    CHECK_EQ(s.hz, kNominal);
    CHECK(s.guest <= s.host);
    CHECK(s.host - s.guest <= GuestPacer::kSlicePs + kMs);  // keeps up closely
}

TEST_CASE("platform/pacer/idle_guest_does_not_run_ahead")
{
    Sim s(40.0, 0.05);  // napping 95% of the time
    while (s.host < 3000 * kMs)
        s.step();
    CHECK(s.ok());
    CHECK(s.guest <= s.host);  // before: naps made guest time race ahead
    CHECK(s.host - s.guest <= GuestPacer::kSlicePs + kMs);
    CHECK_EQ(s.hz, kNominal);  // naps tell nothing about speed: unchanged
}

TEST_CASE("platform/pacer/backlog_is_dropped_not_burst")
{
    GuestPacer p;
    p.start(0, 0, kNominal);
    // The host stalls for 2 s (say, an SD card write): no burst afterwards.
    const u64 run = p.plan(2000 * kMs, 0);
    CHECK(run <= GuestPacer::kSlicePs);
    CHECK(p.dropped_ps() >= 2000 * kMs - GuestPacer::kMaxLagPs);
    // From now the lag is at most kMaxLagPs.
    u64 total = 0;
    for (u64 g = 0; (total = p.plan(2000 * kMs, g)) != 0; g += total) {
    }
    CHECK(p.dropped_ps() <= 2000 * kMs);
}
