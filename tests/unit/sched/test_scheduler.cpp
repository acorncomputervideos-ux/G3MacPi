// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/sched/scheduler and core/machine/execution_loop.

#include "core/cpu/ppc750/ppc750.h"
#include "core/machine/execution_loop.h"
#include "core/mem/flat_memory.h"
#include "core/sched/scheduler.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

#include <string>

using namespace g3;

namespace {
std::string g_trace;
void mark(void* ctx, u64) { g_trace += static_cast<const char*>(ctx); }
} // namespace

TEST_CASE("sched/scheduler/order_and_ties")
{
    g_trace.clear();
    Scheduler s;
    s.schedule_at(300, mark, (void*)"C");
    s.schedule_at(100, mark, (void*)"A");
    s.schedule_at(200, mark, (void*)"B1");
    s.schedule_at(200, mark, (void*)"B2");  // same time: FIFO
    CHECK_EQ(s.next_event_time(), 100u);
    s.advance_to(250);
    CHECK(g_trace == "AB1B2");
    CHECK_EQ(s.now(), 250u);
    s.advance_to(1000);
    CHECK(g_trace == "AB1B2C");
    CHECK_EQ(s.next_event_time(), Scheduler::kNever);
}

TEST_CASE("sched/scheduler/cancel_and_stale_handles")
{
    g_trace.clear();
    Scheduler s;
    const auto h = s.schedule_in(50, mark, (void*)"X");
    CHECK(s.pending(h));
    CHECK(s.cancel(h));
    CHECK(!s.cancel(h));
    s.advance_to(100);
    CHECK(g_trace.empty());
    // Slot reuse must not revive the old handle.
    const auto h2 = s.schedule_in(10, mark, (void*)"Y");
    CHECK(!s.pending(h));
    CHECK(s.pending(h2));
    CHECK(h != h2);
}

TEST_CASE("sched/scheduler/callbacks_can_reschedule")
{
    // A periodic timer rescheduling itself every 10 ps.
    struct Periodic {
        Scheduler* s;
        int count = 0;
        static void tick(void* ctx, u64)
        {
            auto* p = static_cast<Periodic*>(ctx);
            if (++p->count < 5)
                p->s->schedule_in(10, tick, p);
        }
    };
    Scheduler s;
    Periodic p{&s};
    s.schedule_in(10, Periodic::tick, &p);
    s.advance_to(1000);
    CHECK_EQ(p.count, 5);
    // An event scheduled for "now" from a callback fires in the same advance.
    g_trace.clear();
    struct Chain {
        Scheduler* s;
        static void first(void* ctx, u64) {
            g_trace += "1";
            static_cast<Chain*>(ctx)->s->schedule_in(0, mark, (void*)"2");
        }
    };
    Chain c{&s};
    s.schedule_in(5, Chain::first, &c);
    s.advance_to(s.now() + 5);
    CHECK(g_trace == "12");
}

TEST_CASE("sched/scheduler/capacity_and_past")
{
    Scheduler s;
    for (u32 i = 0; i < Scheduler::kMaxEvents; ++i)
        CHECK(s.schedule_in(100 + i, mark, (void*)"") != Scheduler::kInvalidHandle);
    CHECK_EQ(s.schedule_in(1, mark, (void*)""), Scheduler::kInvalidHandle);
    s.advance_to(50);
    CHECK_EQ(s.schedule_at(10, mark, (void*)""), Scheduler::kInvalidHandle);  // in the past
}

// --- execution loop ------------------------------------------------------------

namespace {
constexpr u64 kCpuHz = 350000000;      // 350 MHz
constexpr u64 kTbHz = 25000000;        // 25 MHz: 14 CPU cycles per tick, 40000 ps
constexpr u64 kTickPs = 40000;

struct LoopMachine {
    u8 ram[0x4000] = {};
    FlatMemory mem{ram, sizeof ram};
    ppc::Ppc750 cpu{mem};
    Scheduler sched;
    ExecutionLoop loop{cpu, sched, CpuTiming{kCpuHz, kTbHz, 1}};
    LoopMachine()
    {
        mem.write32(0x1000, asm_ppc::b(0));    // b .   (spin)
        mem.write32(0x900, asm_ppc::b(0));     // decrementer handler: spin
        cpu.state().pc = 0x1000;
        cpu.state().msr = 0;
    }
};
} // namespace

TEST_CASE("machine/loop/timebase_has_no_drift")
{
    LoopMachine m;
    // 10 ms of guest time in awkward slices.
    const u64 total = 10000000000ull;  // ps
    u64 done = 0;
    u64 step = 777777;
    while (done < total) {
        const u64 d = (total - done < step) ? total - done : step;
        m.loop.run_for(d);
        done += d;
        step = step * 3 % 9999991 + 1;
    }
    CHECK_EQ(m.cpu.state().tb, 250000u);  // exactly 25 MHz * 10 ms
    // 350 MHz * 10 ms = 3.5 M instructions (the last slice may overshoot
    // by one instruction).
    const u64 n = m.cpu.instructions_executed();
    CHECK(n >= 3500000u && n <= 3500001u);
}

TEST_CASE("machine/loop/decrementer_fires_on_time")
{
    LoopMachine m;
    m.cpu.state().spr[ppc::spr::DEC] = 100;  // bit 0 flips after 101 ticks
    m.cpu.state().msr = ppc::msr::EE;
    m.loop.run_for(101 * kTickPs - 1);
    CHECK_EQ(m.cpu.state().pc, 0x1000u);  // not yet
    m.loop.run_for(2 * kTickPs);
    CHECK_EQ(m.cpu.state().pc, 0x900u);
    CHECK_EQ(m.cpu.state().spr[ppc::spr::SRR0], 0x1000u);
    // Taken within one instruction of the tick boundary: ~14 instructions
    // per tick at 350 MHz / 25 MHz, so 101 ticks ~ 1414 instructions.
    const u64 n = m.cpu.instructions_executed();
    CHECK(n >= 1414u && n <= 1414u + 2u * 14u + 2u);
}

TEST_CASE("machine/loop/scheduled_event_runs_at_its_time")
{
    LoopMachine m;
    struct Probe {
        LoopMachine* m;
        u64 fired_at = 0;
        u64 instructions = 0;
        static void fire(void* ctx, u64 now)
        {
            auto* p = static_cast<Probe*>(ctx);
            p->fired_at = now;
            p->instructions = p->m->cpu.instructions_executed();
        }
    };
    Probe p{&m};
    m.sched.schedule_at(1000000, Probe::fire, &p);  // 1 microsecond
    m.loop.run_for(5000000);
    CHECK(p.fired_at >= 1000000u && p.fired_at < 1000000u + 2858u);  // within one instruction
    CHECK(p.instructions >= 350u && p.instructions <= 351u);         // 1 us at 350 MHz
}

TEST_CASE("machine/loop/time_moves_within_a_slice")
{
    // A guest loop reads the time base several times inside one slice (no
    // events, no decrementer): each read must see the time of its own
    // instruction, not the slice start. Mac OS calibrates delays this way.
    using namespace asm_ppc;
    LoopMachine m;
    const u32 prog[] = {
        mftb(3, ppc::spr::TBL_READ),      // 0x1000
        li(9, 0x100), mtspr(ppc::spr::CTR, 9),
        bdnz(0),                          // 0x100 iterations of nothing
        mftb(4, ppc::spr::TBL_READ),
        b(0),
    };
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        m.mem.write32(0x1000 + 4 * i, prog[i]);
    m.cpu.state().spr[ppc::spr::DEC] = 0x80000000u;   // no decrementer expiry planned
    m.loop.set_max_slice(1000000);
    m.loop.run_for(20000000);                          // 20 us: one slice
    const u32 t0 = m.cpu.state().gpr[3], t1 = m.cpu.state().gpr[4];
    // ~259 instructions between the reads at 350 MHz = ~740 ns = ~18 ticks at 25 MHz.
    CHECK(t1 > t0);
    CHECK(t1 - t0 >= 17u && t1 - t0 <= 20u);
    // The scheduler's precise clock follows the CPU too.
    CHECK(m.sched.precise_now() == m.loop.now_ps());   // between slices: the same
}

TEST_CASE("machine/loop/decrementer_written_mid_slice")
{
    // mtdec inside a slice: the expiry is planned from the new value.
    using namespace asm_ppc;
    LoopMachine m;
    const u32 prog[] = {li(5, 50), mtspr(ppc::spr::DEC, 5), b(0)};
    for (u32 i = 0; i < sizeof prog / 4; ++i)
        m.mem.write32(0x1000 + 4 * i, prog[i]);
    m.cpu.state().spr[ppc::spr::DEC] = 0x7FFFFFFFu;   // far away when the slice was planned
    m.cpu.state().msr = ppc::msr::EE;
    m.loop.set_max_slice(1000000);
    struct At { ppc::Ppc750* cpu; u64 n = 0; } at{&m.cpu};
    m.cpu.set_exception_hook([](void* c, u32 vector, u32, u32) {
        auto* a = static_cast<At*>(c);
        if (vector == 0x900 && !a->n)
            a->n = a->cpu->instructions_executed();
    }, &at);
    m.loop.run_for(1000000000ull);                     // 1 ms
    CHECK_EQ(m.cpu.state().pc, 0x900u);
    // Taken ~51 ticks (~714 instructions) after the write, not at the end of the run.
    CHECK(at.n >= 700u && at.n <= 760u);
}

TEST_CASE("machine/loop/dozing_cpu_lets_time_pass")
{
    // Mac OS's idle loop: mtmsr with MSR[POW] and EE, HID0[DOZE] set. The
    // CPU stops (750 UM 10.2) and guest time goes straight to the
    // decrementer interrupt that wakes it, without instructions.
    for (int doze = 0; doze < 2; ++doze) {
        LoopMachine m;
        m.mem.write32(0x1000, asm_ppc::mtmsr(10));
        m.mem.write32(0x1004, asm_ppc::b(-4));
        m.cpu.state().gpr[10] = ppc::msr::POW | ppc::msr::EE;
        m.cpu.state().spr[ppc::spr::HID0] = doze ? ppc::hid0::DOZE : 0;
        m.cpu.state().spr[ppc::spr::DEC] = 100;
        m.loop.run_for(101 * kTickPs - 1);
        CHECK(m.cpu.state().pc == 0x1000u || m.cpu.state().pc == 0x1004u);  // not woken yet
        CHECK_EQ(m.cpu.state().tb, 100u);  // time went on all the same
        if (doze) {
            CHECK(m.cpu.dozing());
            CHECK(m.cpu.instructions_executed() <= 2u);  // one mtmsr: then asleep
        } else {
            // Without a power-saving mode in HID0, MSR[POW] does nothing: it spins.
            CHECK(!m.cpu.dozing());
            CHECK(m.cpu.instructions_executed() >= 1400u);
        }
        m.loop.run_for(2 * kTickPs);
        CHECK_EQ(m.cpu.state().pc, 0x900u);      // the decrementer woke it
        CHECK_EQ(m.cpu.state().msr & ppc::msr::POW, 0u);
        CHECK(!m.cpu.dozing());
    }
}
