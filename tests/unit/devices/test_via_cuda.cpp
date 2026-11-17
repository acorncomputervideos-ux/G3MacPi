// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the VIA 6522 and the Cuda.
//
// The Cuda tests use, as the "host", a re-implementation of the algorithm of
// the Linux driver drivers/macintosh/via-cuda.c (cuda_init_via, cuda_start,
// cuda_interrupt states), talking to our Cuda only through VIA registers on
// the physical bus, with guest time advancing between interrupts. If Linux's
// own protocol logic works against our Cuda, the protocol is right.

#include "core/devices/macio/cuda.h"
#include "core/devices/macio/via6522.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;

namespace {

constexpr u32 kViaBase = 0xF3016000u;  // arbitrary for the tests
constexpr u32 RS = 0x200;
enum { B = 0, DIRB = 2 * RS, T1CL = 4 * RS, T1CH = 5 * RS, T1LL = 6 * RS, T1LH = 7 * RS,
       T2CL = 8 * RS, T2CH = 9 * RS, SR = 10 * RS, ACR = 11 * RS, IFR = 13 * RS, IER = 14 * RS };
// Ticks of the 783.333 kHz VIA clock in picoseconds (rounded up).
constexpr u64 kTickPs = 1276596;

struct Rig {
    Scheduler sched;
    PhysicalBus bus;
    Via6522 via{sched};
    bool irq = false;
    Rig()
    {
        bus.map_mmio(kViaBase, Via6522::kWindowSize, &via);
        via.set_irq_output([](void* c, bool a) { static_cast<Rig*>(c)->irq = a; }, this);
    }
    u8 in(u32 reg)
    {
        u8 v = 0;
        bus.read8(kViaBase + reg, v);
        return v;
    }
    void out(u32 reg, u8 v) { bus.write8(kViaBase + reg, v); }
};

} // namespace

// --- VIA ----------------------------------------------------------------------------

TEST_CASE("devices/via/register_spacing_and_ier_ifr")
{
    Rig r;
    r.out(IER, 0x80 | Via6522::T1 | Via6522::SR_INT);
    CHECK_EQ(r.in(IER), 0x80u | Via6522::T1 | Via6522::SR_INT);  // bit 7 reads as 1
    r.out(IER, Via6522::T1);                                      // bit 7 clear: disable
    CHECK_EQ(r.in(IER), 0x80u | Via6522::SR_INT);
    r.via.raise_interrupt(Via6522::SR_INT);
    CHECK(r.irq);
    CHECK_EQ(r.in(IFR), 0x80u | Via6522::SR_INT);                 // bit 7 = any enabled
    r.out(IFR, Via6522::SR_INT);                                  // write 1 to clear
    CHECK(!r.irq);
    CHECK_EQ(r.in(IFR), 0u);
    r.out(DIRB, 0x30);
    CHECK_EQ(r.in(DIRB), 0x30u);
}

TEST_CASE("devices/via/timer1_one_shot")
{
    Rig r;
    r.out(IER, 0x80 | Via6522::T1);
    r.out(T1CL, 0x10);
    r.out(T1CH, 0x00);                 // start: 16 ticks -> interrupt after 17
    r.sched.advance_to(10 * kTickPs);
    CHECK_EQ(r.in(T1CL), 6u);          // counting down
    CHECK(!r.irq);
    r.sched.advance_to(18 * kTickPs);
    CHECK(r.irq);
    CHECK(r.in(IFR) & Via6522::T1);
    r.in(T1CL);                        // reading T1CL clears the flag
    CHECK(!r.irq);
    r.sched.advance_to(200 * kTickPs); // one-shot: no further interrupt
    CHECK(!r.irq);
}

TEST_CASE("devices/via/timer1_free_running_and_timer2")
{
    Rig r;
    r.out(IER, 0x80 | Via6522::T1 | Via6522::T2);
    r.out(ACR, 0x40);                  // T1 continuous
    r.out(T1CL, 99);
    r.out(T1CH, 0);
    int count = 0;
    for (u64 t = 1; t <= 1000; ++t) {
        r.sched.advance_to(t * kTickPs);
        if (r.in(IFR) & Via6522::T1) {
            ++count;
            r.in(T1CL);
        }
    }
    CHECK_EQ(count, 10);               // every 100 ticks over 1000 ticks
    // Timer 2: one-shot, 50 ticks.
    r.out(T2CL, 50);
    r.out(T2CH, 0);
    r.sched.advance_to(r.sched.now() + 52 * kTickPs);
    CHECK(r.in(IFR) & Via6522::T2);
}

// --- Cuda through the Linux driver algorithm ---------------------------------------

namespace {

constexpr u8 TREQ = 0x08, TACK = 0x10, TIP = 0x20;
constexpr u8 SR_CTRL = 0x1c, SR_EXT = 0x0c, SR_OUT = 0x10, SR_INT = 0x04;

struct LinuxCudaHost {
    Rig& r;
    explicit LinuxCudaHost(Rig& rig) : r(rig) {}

    enum class St { idle, sent_first_byte, sending, reading, read_done, awaiting_reply };
    St state = St::idle;
    std::vector<u8> req, reply, unsolicited;
    size_t data_index = 0;
    bool reply_expected = false, done = false, reading_reply = false;
    bool req_pending = false;  // Linux: current_req queued but not yet sent

    bool treq_asserted() { return !(r.in(B) & TREQ); }
    void assert_tip() { r.out(B, r.in(B) & ~TIP); }
    void assert_tack() { r.out(B, r.in(B) & ~TACK); }
    void negate_tack() { r.out(B, r.in(B) | TACK); }
    void toggle_tack() { r.out(B, r.in(B) ^ TACK); }
    void negate_tip_and_tack() { r.out(B, r.in(B) | TIP | TACK); }

    // Advances guest time until the VIA flags SR_INT (bounded).
    bool wait_sr_int()
    {
        for (int i = 0; i < 1000; ++i) {
            if (r.in(IFR) & SR_INT)
                return true;
            const u64 next = r.sched.next_event_time();
            if (next == Scheduler::kNever)
                return false;
            r.sched.advance_to(next);
        }
        return false;
    }

    // cuda_init_via()
    bool init_via()
    {
        r.out(IER, 0x7f);
        r.in(IER);
        r.out(DIRB, (r.in(DIRB) | TACK | TIP) & ~TREQ);
        r.out(ACR, (r.in(ACR) & ~SR_CTRL) | SR_EXT);
        r.in(SR);
        negate_tip_and_tack();
        r.sched.advance_to(r.sched.now() + 4000000000ull);  // mdelay(4)
        r.in(SR);
        r.out(IFR, SR_INT);
        assert_tack();
        if (!treq_asserted() || !wait_sr_int())
            return false;
        r.in(SR);
        r.out(IFR, SR_INT);
        negate_tack();
        if (treq_asserted() || !wait_sr_int())
            return false;
        r.in(SR);
        r.out(IFR, SR_INT);
        return true;
    }

    // cuda_start()
    void start()
    {
        req_pending = false;
        r.out(ACR, r.in(ACR) | SR_OUT);
        r.out(SR, req[0]);
        assert_tip();
        data_index = 1;
        state = St::sent_first_byte;
    }

    // cuda_interrupt() state machine
    void interrupt()
    {
        r.out(IFR, SR_INT);
        switch (state) {
        case St::idle:  // unsolicited data
            r.in(SR);
            assert_tip();
            reading_reply = false;
            state = St::reading;
            break;
        case St::sent_first_byte:
            if (treq_asserted()) {  // collision: request stays queued
                r.out(ACR, r.in(ACR) & ~SR_OUT);
                r.in(SR);
                negate_tip_and_tack();
                req_pending = true;
                state = St::idle;
            } else {
                r.out(SR, req[data_index++]);
                toggle_tack();
                state = St::sending;
            }
            break;
        case St::sending:
            if (data_index >= req.size()) {
                r.out(ACR, r.in(ACR) & ~SR_OUT);
                r.in(SR);
                negate_tip_and_tack();
                if (reply_expected) {
                    state = St::awaiting_reply;
                } else {
                    done = true;
                    state = St::idle;
                }
            } else {
                r.out(SR, req[data_index++]);
                toggle_tack();
            }
            break;
        case St::awaiting_reply:
            r.in(SR);
            assert_tip();
            reading_reply = true;
            state = St::reading;
            break;
        case St::reading:
            (reading_reply ? reply : unsolicited).push_back(r.in(SR));
            if (!treq_asserted()) {
                negate_tip_and_tack();
                state = St::read_done;
            } else {
                toggle_tack();
            }
            break;
        case St::read_done:
            r.in(SR);
            if (reading_reply)
                done = true;
            state = St::idle;
            // Linux: cuda_start() for a queued request (retry after collision).
            if (req_pending && !treq_asserted())
                start();
            break;
        }
    }

    // A whole request with a reply, like cuda_request() + polling.
    bool request(std::vector<u8> bytes)
    {
        req = std::move(bytes);
        reply.clear();
        done = false;
        reply_expected = true;
        start();
        for (int i = 0; i < 1000 && !done; ++i) {
            if (!wait_sr_int())
                return false;
            interrupt();
        }
        return done;
    }
};

struct CudaRig {
    Rig rig;
    Cuda cuda{rig.via, rig.sched};
    LinuxCudaHost host{rig};
    AdbBus adb;
    AdbKeyboard kbd;
    AdbMouse mouse;
    CudaRig()
    {
        adb.attach(&kbd);
        adb.attach(&mouse);
        cuda.attach_adb(&adb);
    }
    // Lets guest time pass, servicing interrupts like the Linux driver.
    void service_until(u64 t)
    {
        for (;;) {
            while (rig.in(IFR) & SR_INT)
                host.interrupt();
            const u64 next = rig.sched.next_event_time();
            if (next == Scheduler::kNever || next > t)
                break;
            rig.sched.advance_to(next);
        }
        rig.sched.advance_to(t);
    }
};

} // namespace

TEST_CASE("devices/cuda/linux_init_sync")
{
    CudaRig c;
    CHECK(c.host.init_via());
}

TEST_CASE("devices/cuda/get_and_set_time")
{
    CudaRig c;
    REQUIRE(c.host.init_via());
    c.cuda.set_rtc(0xB4C3D2E1u);
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kGetTime}));
    // Linux checks reply_len == 7 and builds the time from reply[3..6].
    REQUIRE(c.host.reply.size() == 7);
    CHECK_EQ(c.host.reply[0], Cuda::kCudaPacket);
    CHECK_EQ(c.host.reply[1], 0u);
    CHECK_EQ(c.host.reply[2], Cuda::kGetTime);
    const u32 t = (c.host.reply[3] << 24) | (c.host.reply[4] << 16) | (c.host.reply[5] << 8) |
                  c.host.reply[6];
    CHECK_EQ(t, 0xB4C3D2E1u);

    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kSetTime, 0x01, 0x02, 0x03, 0x04}));
    CHECK_EQ(c.host.reply.size(), 3u);  // Linux accepts 3 or 7
    CHECK_EQ(c.cuda.rtc(), 0x01020304u);
    // The clock runs with guest time.
    c.rig.sched.advance_to(c.rig.sched.now() + 3 * 1000000000000ull);
    CHECK_EQ(c.cuda.rtc(), 0x01020307u);
}

TEST_CASE("devices/cuda/pram_autopoll_errors_adb")
{
    CudaRig c;
    REQUIRE(c.host.init_via());
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kSetPram, 0x00, 0x10, 0xAB, 0xCD}));
    CHECK_EQ(c.cuda.pram()[0x10], 0xABu);
    CHECK_EQ(c.cuda.pram()[0x11], 0xCDu);
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kGetPram, 0x00, 0x11}));
    REQUIRE(c.host.reply.size() == 4);
    CHECK_EQ(c.host.reply[3], 0xCDu);

    // Linux enables autopoll with (CUDA_PACKET, CUDA_AUTOPOLL, 1).
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kAutopoll, 1}));
    CHECK(c.cuda.autopoll_enabled());

    // Unknown command -> ERROR_PACKET, 2, CUDA_PACKET, cmd.
    REQUIRE(c.host.request({Cuda::kCudaPacket, 0x77}));
    REQUIRE(c.host.reply.size() == 4);
    CHECK_EQ(c.host.reply[0], Cuda::kErrorPacket);
    CHECK_EQ(c.host.reply[1], 2u);
    CHECK_EQ(c.host.reply[3], 0x77u);

    // ADB talk to the mouse (address 3, register 0) with no motion: the
    // device does not answer -> flag 0x02.
    REQUIRE(c.host.request({Cuda::kAdbPacket, 0x3C}));
    REQUIRE(c.host.reply.size() == 3);
    CHECK_EQ(c.host.reply[0], Cuda::kAdbPacket);
    CHECK(c.host.reply[1] & 0x02);
}

TEST_CASE("devices/cuda/power_commands_and_many_requests")
{
    CudaRig c;
    REQUIRE(c.host.init_via());
    struct Flags { int down = 0, reset = 0; } f;
    c.cuda.set_power_callbacks([](void* x) { ++static_cast<Flags*>(x)->down; },
                               [](void* x) { ++static_cast<Flags*>(x)->reset; }, &f);
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kPowerDown}));
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kResetSystem}));
    CHECK_EQ(f.down, 1);
    CHECK_EQ(f.reset, 1);
    // Robustness: many back-to-back requests stay in sync.
    for (int i = 0; i < 200; ++i) {
        REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kGetAutoRate}));
        REQUIRE(c.host.reply.size() == 4);
    }
    CHECK(c.host.unsolicited.empty());
}

TEST_CASE("devices/cuda/adb_talk_through_cuda")
{
    CudaRig c;
    REQUIRE(c.host.init_via());
    // Talk register 3 of the keyboard: address 2, handler 1.
    REQUIRE(c.host.request({Cuda::kAdbPacket, 0x2F}));
    REQUIRE(c.host.reply.size() == 5);
    CHECK_EQ(c.host.reply[1], 0u);
    CHECK_EQ(c.host.reply[3] & 0x0F, 2u);
    CHECK_EQ(c.host.reply[4], 1u);
    // A key press, read with Talk R0.
    c.kbd.key_event(0x24, true);  // Return
    REQUIRE(c.host.request({Cuda::kAdbPacket, 0x2C}));
    REQUIRE(c.host.reply.size() == 5);
    CHECK_EQ(c.host.reply[3], 0x24u);
    CHECK_EQ(c.host.reply[4], 0xFFu);
}

TEST_CASE("devices/cuda/autopoll_delivers_unsolicited_packets")
{
    CudaRig c;
    REQUIRE(c.host.init_via());
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kAutopoll, 1}));
    c.kbd.key_event(0x00, true);   // 'A' pressed
    c.mouse.motion(4, 2);
    c.service_until(c.rig.sched.now() + 50 * 1000000000ull);  // 50 ms
    // Two unsolicited ADB packets: 0x40 = autopoll flag, then cmd and data.
    REQUIRE(c.host.unsolicited.size() == 10);
    CHECK_EQ(c.host.unsolicited[0], Cuda::kAdbPacket);
    CHECK_EQ(c.host.unsolicited[1], 0x40u);
    CHECK_EQ(c.host.unsolicited[2], 0x2Cu);            // keyboard Talk R0
    CHECK_EQ(c.host.unsolicited[3], 0x00u);            // 'A' down
    CHECK_EQ(c.host.unsolicited[7], 0x3Cu);            // mouse Talk R0
    CHECK_EQ(c.host.unsolicited[8], 0x82u);            // button up, dy = 2
    CHECK_EQ(c.host.unsolicited[9], 0x84u);            // button up, dx = 4
}

TEST_CASE("devices/cuda/collision_host_request_vs_autopoll")
{
    CudaRig c;
    REQUIRE(c.host.init_via());
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kAutopoll, 1}));
    c.kbd.key_event(0x31, true);
    // Run until the autopoll attention is raised (TREQ asserted), but do
    // not service it yet: the host starts a request right now.
    for (int i = 0; i < 100 && !c.host.treq_asserted(); ++i)
        c.rig.sched.advance_to(c.rig.sched.next_event_time());
    REQUIRE(c.host.treq_asserted());
    REQUIRE(c.host.request({Cuda::kCudaPacket, Cuda::kGetAutoRate}));  // must still succeed
    CHECK_EQ(c.host.reply.size(), 4u);
    c.service_until(c.rig.sched.now() + 20 * 1000000000ull);
    REQUIRE(c.host.unsolicited.size() >= 5);
    CHECK_EQ(c.host.unsolicited[3], 0x31u);  // the key press was not lost
}
