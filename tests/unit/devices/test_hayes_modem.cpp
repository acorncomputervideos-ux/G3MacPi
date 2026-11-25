// SPDX-License-Identifier: GPL-3.0-or-later
// The virtual Hayes modem (core/devices/modem/hayes_modem.h): commands,
// results, dialling and its timing, carrier, data, escape, DTR, hang-ups.

#include "core/devices/modem/hayes_modem.h"
#include "tests/framework/g3test.h"

#include <string>
#include <vector>

using namespace g3;

namespace {
constexpr u64 kMs = 1000;

struct Rig {
    HayesModem m;
    u64 now = 0;
    std::string line_data;
    std::vector<bool> line_states;
    std::string events;  // one letter per event (digits as themselves)

    Rig()
    {
        m.set_events([](void* c, HayesModem::Event e, char d) {
            auto* r = static_cast<Rig*>(c);
            switch (e) {
            case HayesModem::Event::OffHook: r->events += 'O'; break;
            case HayesModem::Event::Digit: r->events += d; break;
            case HayesModem::Event::Pause: r->events += '_'; break;
            case HayesModem::Event::Ringback: r->events += 'R'; break;
            case HayesModem::Event::Handshake: r->events += 'H'; break;
            case HayesModem::Event::Connected: r->events += 'C'; break;
            case HayesModem::Event::OnHook: r->events += 'X'; break;
            }
        }, this);
        m.set_line([](void* c, const u8* d, u32 n) { static_cast<Rig*>(c)->line_data.append(reinterpret_cast<const char*>(d), n); },
                   [](void* c, bool up) { static_cast<Rig*>(c)->line_states.push_back(up); }, this);
    }
    void type(const std::string& s, u64 gap_ms = 0)
    {
        for (char c : s) {
            m.from_mac(static_cast<u8>(c), now);
            now += gap_ms * kMs;
            m.tick(now);
        }
    }
    void wait(u64 ms)
    {
        now += ms * kMs;
        m.tick(now);
    }
    std::string out()
    {
        std::string s;
        u8 b[64];
        while (u32 n = m.to_mac(b, sizeof b))
            s.append(reinterpret_cast<char*>(b), n);
        return s;
    }
    void connect()
    {
        type("ATE0\r");
        out();
        type("ATD1\r");
        wait(60000);
        out();
    }
};
} // namespace

TEST_CASE("devices/modem/commands_and_results")
{
    Rig r;
    r.type("AT\r");
    CHECK(r.out() == "AT\r\r\nOK\r\n");
    r.type("ATE0\r");
    CHECK(r.out() == "ATE0\r\r\nOK\r\n");          // echoed while typed, then off
    r.type("at\r");
    CHECK(r.out() == "\r\nOK\r\n");
    r.type("hello\r");
    CHECK(r.out().empty());                         // not a command line
    r.type("ATE7\r");
    CHECK(r.out() == "\r\nERROR\r\n");
    r.type("ATV0\r");
    CHECK(r.out() == "0\r");
    r.type("ATE5\r");
    CHECK(r.out() == "4\r");
    r.type("ATV1Q1\r");
    CHECK(r.out().empty());                         // quiet
    r.type("ATQ0S0=2\r");
    CHECK(r.out() == "\r\nOK\r\n");
    r.type("ATS0?\r");
    CHECK(r.out() == "\r\n002\r\n\r\nOK\r\n");
    r.type("ATS99=1\r");
    CHECK(r.out() == "\r\nERROR\r\n");
    // A vendor init string: the unknown parts are accepted.
    r.type("AT&FE0V1&C1&D2\\N3%C1+MS=V90;S7=60\r");
    CHECK(r.out() == "\r\nOK\r\n");
    CHECK_EQ(r.m.s(7), 60u);
    // A/ repeats the last line at once; backspace edits.
    r.type("ATS1=9\r");
    r.out();
    r.type("A/");
    CHECK(r.out() == "\r\nOK\r\n");
    r.type("ATS1=3\x08" "4\r");
    CHECK_EQ(r.m.s(1), 4u);
    r.out();
    // ATZ: factory settings (echo on again).
    r.type("ATZ\r");
    CHECK(r.out() == "\r\nOK\r\n");
    r.type("AT\r");
    CHECK(r.out() == "AT\r\r\nOK\r\n");
    r.type("ATI3\r");
    CHECK(r.out() == "ATI3\r\r\nG3MacPi virtual modem\r\n\r\nOK\r\n");
}

TEST_CASE("devices/modem/dial_sequence_and_timing")
{
    Rig r;
    r.type("ATE0\r");
    r.out();
    CHECK(!r.m.dcd());                              // &C1: no carrier yet
    r.type("ATDT55,1\r");
    CHECK(r.events == "O");
    // 1.5 s dial tone; DTMF 95 ms + 95 ms gap per digit; ',' 2 s; ring 4 s;
    // handshake 12 s: connected at 20 070 ms.
    r.wait(1499);
    CHECK(r.events == "O");
    r.wait(1);
    CHECK(r.events == "O5");
    r.wait(20069 - 1500);
    CHECK(r.events == "O5_5__1_RH");
    CHECK(r.out().empty());
    CHECK(!r.m.dcd());
    r.wait(1);
    CHECK(r.events == "O5_5__1_RHC");
    CHECK(r.out() == "\r\nCONNECT 53333\r\n");
    CHECK(r.m.dcd());
    REQUIRE(r.line_states.size() == 1u);
    CHECK(r.line_states[0]);
    // X0: plain CONNECT.
    Rig q;
    q.type("ATE0X0\r");
    q.out();
    q.type("ATD1\r");
    q.wait(60000);
    CHECK(q.out() == "\r\nCONNECT\r\n");
    // &C0: carrier always reported.
    Rig c0;
    c0.type("AT&C0\r");
    CHECK(c0.m.dcd());
    // Pulse: 2 pulses of 100 ms, then 700 ms between digits.
    Rig p;
    p.type("ATE0DP2\r");
    p.wait(1500);
    CHECK(p.events == "O2");
    p.wait(199);
    CHECK(p.events == "O2");
    p.wait(1);
    CHECK(p.events == "O2_");
    p.wait(699);
    CHECK(p.events == "O2_");
    p.wait(1);
    CHECK(p.events == "O2_R");
    // ';': dial, then back to commands without connecting.
    Rig s;
    s.type("ATE0\r");
    s.out();
    s.type("ATD12;\r");
    s.wait(5000);
    CHECK(s.out() == "\r\nOK\r\n");
    CHECK(!s.m.connected());
    // Nothing to dial.
    Rig e;
    e.type("ATE0\r");
    e.out();
    e.type("ATD\r");
    CHECK(e.out() == "\r\nNO CARRIER\r\n");
}

TEST_CASE("devices/modem/online_data_and_escape")
{
    Rig r;
    r.connect();
    CHECK(r.m.online());
    r.type("~\x7d" "ab");
    CHECK(r.line_data == "~\x7d" "ab");
    const u8 reply[] = {'x', 'y'};
    r.m.from_line(reply, 2);
    CHECK(r.out() == "xy");
    // "+++" inside data (no silence before): passed on, no escape.
    r.type("a+++", 10);
    r.wait(2000);
    CHECK(r.m.online());
    CHECK(r.out().empty());
    // With a second of silence on both sides: command mode, still connected.
    r.wait(1100);
    r.type("+++", 100);                             // (type waits 100 ms after the last too)
    r.wait(899);
    CHECK(r.m.online());                            // 999 ms after the third '+'
    r.wait(2);
    CHECK(!r.m.online());
    CHECK(r.m.connected());
    CHECK(r.out() == "\r\nOK\r\n");
    CHECK(r.m.dcd());
    r.m.from_line(reply, 2);
    CHECK(r.out().empty());                         // not passed on in command mode
    r.type("ATO\r");
    CHECK(r.out() == "\r\nCONNECT 53333\r\n");
    CHECK(r.m.online());
    // Escape again, then hang up.
    r.wait(1100);
    r.type("+++", 100);
    r.wait(1100);
    r.out();
    r.type("ATH\r");
    CHECK(r.out() == "\r\nOK\r\n");
    CHECK(!r.m.connected());
    CHECK(!r.m.dcd());
    REQUIRE(r.line_states.size() == 2u);
    CHECK(!r.line_states[1]);
    // Four pluses are not an escape.
    Rig f;
    f.connect();
    f.wait(1100);
    f.type("++++", 100);
    f.wait(2000);
    CHECK(f.m.online());
}

TEST_CASE("devices/modem/hang_ups")
{
    // DTR drop, &D2 (default): hang up.
    Rig r;
    r.m.set_dtr(true, r.now);
    r.connect();
    r.m.set_dtr(false, r.now);
    CHECK(!r.m.connected());
    CHECK(r.events.back() == 'X');
    // &D1: command mode, still connected.
    Rig d1;
    d1.m.set_dtr(true, d1.now);
    d1.type("AT&D1\r");
    d1.connect();
    d1.m.set_dtr(false, d1.now);
    CHECK(d1.m.connected());
    CHECK(!d1.m.online());
    CHECK(d1.out() == "\r\nOK\r\n");
    // &D0: ignored.
    Rig d0;
    d0.m.set_dtr(true, d0.now);
    d0.type("AT&D0\r");
    d0.connect();
    d0.m.set_dtr(false, d0.now);
    CHECK(d0.m.online());
    // A key while dialling aborts.
    Rig a;
    a.type("ATE0D123\r");
    a.out();
    a.wait(2000);
    a.type("x");
    CHECK(a.out() == "\r\nNO CARRIER\r\n");
    CHECK(!a.m.connected());
    a.wait(60000);
    CHECK(!a.m.connected());
    // The provider ends the call.
    Rig p;
    p.connect();
    p.m.remote_hang_up(p.now);
    CHECK(p.out() == "\r\nNO CARRIER\r\n");
    CHECK(!p.m.dcd());
}
