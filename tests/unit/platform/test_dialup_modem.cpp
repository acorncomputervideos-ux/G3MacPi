// SPDX-License-Identifier: GPL-3.0-or-later
// The dial-up modem put together (platform/common/dialup_modem.h), driven
// through the SCC's registers as Mac OS's serial driver would: AT commands,
// pacing, dialling, carrier, the PPP provider's first frame, DTR hang-up.

#include "platform/common/dialup_modem.h"
#include "core/machine/yosemite.h"
#include "tests/framework/g3test.h"

#include <string>

using namespace g3;

namespace {
constexpr u64 kUsPs = 1000000;
constexpr u32 kACtl = 0x80813020u, kAData = 0x80813030u;

struct Rig {
    static u8 ram[64 * 1024];
    YosemiteMachine m;
    DialupModem* d = nullptr;
    const u8 mac[6] = {0x02, 0x47, 0x33, 0x4D, 0x50, 0x01};
    Rig() : m(cfg())
    {
        Paddington& p = m.macio();
        p.config_write(0x10, 0x80800000u, 0xF);
        p.config_write(0x04, 0x2, 0x3);
        d = new DialupModem(p.escc(), m.scheduler(), mac);
        wreg(9, 0xC0);                       // reset
        wreg(4, 0x44);                       // x16, 1 stop bit
        wreg(12, 0);
        wreg(13, 0);
        wreg(14, 0x01);                      // 57 600 baud
        wreg(3, 0xC1);                       // Rx on, 8 bits
        wreg(5, 0xEA);                       // DTR, Tx on, 8 bits, RTS
    }
    ~Rig() { delete d; }
    static YosemiteConfig cfg()
    {
        YosemiteConfig c;
        c.ram = ram;
        c.ram_size = sizeof ram;
        return c;
    }
    void wreg(u32 n, u8 v)
    {
        m.bus().write8(kACtl, static_cast<u8>((n & 7) | (n >= 8 ? 0x08 : 0)));
        m.bus().write8(kACtl, v);
    }
    u8 rr0()
    {
        u8 v = 0;
        m.bus().read8(kACtl, v);
        return v;
    }
    void type(const char* s)
    {
        for (; *s; ++s)
            m.bus().write8(kAData, static_cast<u8>(*s));
    }
    // Advances time, reading what arrives as the driver would (polling).
    std::string run(u64 us)
    {
        std::string got;
        const u64 end = m.scheduler().now() + us * kUsPs;
        while (m.scheduler().now() < end) {
            m.scheduler().advance_to(m.scheduler().now() + 20 * kUsPs);
            while (rr0() & 0x01) {
                u8 b = 0;
                m.bus().read8(kAData, b);
                got.push_back(static_cast<char>(b));
            }
        }
        return got;
    }
};
u8 Rig::ram[64 * 1024];
} // namespace

TEST_CASE("platform/dialup_modem/through_the_scc")
{
    Rig r;
    r.type("AT\r");
    CHECK(r.run(5000) == "AT\r\r\nOK\r\n");
    // Paced as the port's speed: 57 600 baud is 5 760 characters a second.
    r.type("ATI3\r");
    const std::string first = r.run(1000);                  // 1 ms: about 5 characters
    CHECK(first.size() >= 4u);
    CHECK(first.size() <= 7u);
    r.run(20000);
    // Dial; no carrier until the handshake is over.
    r.type("ATE0\r");
    r.run(5000);
    CHECK_EQ(r.rr0() & 0x08, 0u);                           // DCD off
    r.type("ATDT5551234\r");
    std::string s = r.run(10000000);
    CHECK(s.empty());
    CHECK_EQ(r.rr0() & 0x08, 0u);
    s = r.run(12000000);
    CHECK(s.find("CONNECT 53333") != std::string::npos);
    CHECK_EQ(r.rr0() & 0x08, 0x08u);                        // DCD on
    CHECK_EQ(r.rr0() & 0x20, 0x20u);                        // CTS on
    // The provider speaks first: an LCP Configure-Request, fully escaped.
    const size_t at = s.find('\x7E');
    REQUIRE(at != std::string::npos);
    CHECK(s.compare(at, 6, std::string("\x7E\xFF\x7D\x23\xC0\x21", 6)) == 0);
    CHECK(r.d->ppp().running());
    // DTR drops: the modem hangs up (&D2), carrier goes.
    r.wreg(5, 0x6A);
    r.run(2000);
    CHECK_EQ(r.rr0() & 0x08, 0u);
    CHECK(!r.d->ppp().running());
}
