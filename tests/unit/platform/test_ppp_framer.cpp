// SPDX-License-Identifier: GPL-3.0-or-later
// PPP HDLC-like framing (platform/common/ppp_framer.h, RFC 1662).

#include "platform/common/ppp_framer.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

namespace {
struct Got {
    std::vector<u16> protocols;
    std::vector<std::vector<u8>> infos;
};
void feed(PppFramer& f, const u8* b, u32 n)
{
    for (u32 i = 0; i < n; ++i)
        f.receive(b[i]);
}
PppFramer make(Got& g)
{
    PppFramer f;
    f.set_receiver([](void* c, u16 p, const u8* d, u32 n) {
        auto* got = static_cast<Got*>(c);
        got->protocols.push_back(p);
        got->infos.emplace_back(d, d + n);
    }, &g);
    return f;
}
} // namespace

TEST_CASE("platform/ppp/fcs16_check_value")
{
    // CRC-16/X-25 (the PPP FCS): check value 0x906E for "123456789".
    const char* s = "123456789";
    const u16 fcs = ppp::fcs16(ppp::kFcsInit, reinterpret_cast<const u8*>(s), 9);
    CHECK_EQ(static_cast<u16>(~fcs), 0x906Eu);
    // Over the data and its (complemented, little-endian) FCS: the good value.
    u8 buf[11];
    std::memcpy(buf, s, 9);
    buf[9] = static_cast<u8>(~fcs);
    buf[10] = static_cast<u8>(static_cast<u16>(~fcs) >> 8);
    CHECK_EQ(ppp::fcs16(ppp::kFcsInit, buf, 11), ppp::kFcsGood);
}

TEST_CASE("platform/ppp/round_trip_and_options")
{
    const u8 info[] = {0x45, 0x00, 0x7E, 0x7D, 0x11, 0x13, 0x00, 0xFF, 0x20, 0x03};
    for (int acfc = 0; acfc < 2; ++acfc)
        for (int pfc = 0; pfc < 2; ++pfc)
            for (u32 accm : {0xFFFFFFFFu, 0u}) {
                Got g;
                PppFramer f = make(g);
                PppFramer::Options o;
                o.acfc = acfc;
                o.pfc = pfc;
                o.accm = accm;
                u8 out[64];
                const u32 n = PppFramer::encode(0x0021, info, sizeof info, o, out);
                feed(f, out, n);
                REQUIRE(g.protocols.size() == 1u);
                CHECK_EQ(g.protocols[0], 0x0021u);
                CHECK(g.infos[0] == std::vector<u8>(info, info + sizeof info));
                // Never a raw flag or escape inside the frame.
                for (u32 i = 1; i + 1 < n; ++i)
                    CHECK(out[i] != 0x7E);
                // Control characters raw exactly when the ACCM allows.
                bool raw11 = false;
                for (u32 i = 1; i + 1 < n; ++i)
                    raw11 |= out[i] == 0x11 && out[i - 1] != 0x7D;
                CHECK_EQ(raw11, accm == 0);
                // Header: FF 03 unless ACFC; one-byte protocol with PFC.
                CHECK_EQ(out[1] == 0xFF, !acfc);
                if (acfc && pfc)
                    CHECK_EQ(out[1], 0x21u);
                if (acfc && !pfc)                    // 0x00 0x21; 0x00 escaped when the ACCM says
                    CHECK(accm ? (out[1] == 0x7D && out[2] == 0x20) : out[1] == 0x00);
            }
}

TEST_CASE("platform/ppp/lcp_always_full_and_escaped")
{
    const u8 info[] = {0x01, 0x01, 0x00, 0x04};
    PppFramer::Options o;
    o.acfc = o.pfc = true;
    o.accm = 0;
    u8 out[64];
    const u32 n = PppFramer::encode(0xC021, info, sizeof info, o, out);
    CHECK_EQ(out[1], 0xFFu);                         // FF 03 kept
    CHECK_EQ(out[2], 0x7Du);                         // 0x03 escaped (all control characters)
    CHECK_EQ(out[3], 0x23u);
    Got g;
    PppFramer f = make(g);
    feed(f, out, n);
    REQUIRE(g.protocols.size() == 1u);
    CHECK_EQ(g.protocols[0], 0xC021u);
}

TEST_CASE("platform/ppp/bad_frames_dropped")
{
    const u8 info[] = {1, 2, 3, 4, 5};
    PppFramer::Options o;
    u8 out[64];
    const u32 n = PppFramer::encode(0x0021, info, sizeof info, o, out);
    Got g;
    PppFramer f = make(g);
    std::vector<u8> bad(out, out + n);
    bad[5] ^= 0x40;                                  // a corrupted byte
    feed(f, bad.data(), static_cast<u32>(bad.size()));
    CHECK(g.protocols.empty());
    CHECK_EQ(f.bad_frames(), 1u);
    const u8 tiny[] = {0x7E, 0x01, 0x7E, 0x7E};      // too short; then an empty one
    feed(f, tiny, sizeof tiny);
    CHECK_EQ(f.bad_frames(), 2u);
    feed(f, out, n);                                 // a good one after them
    CHECK_EQ(g.protocols.size(), 1u);
    // Shared flags between frames (one 0x7E ends one and starts the next).
    Got g2;
    PppFramer f2 = make(g2);
    std::vector<u8> two(out, out + n);
    two.insert(two.end(), out + 1, out + n);
    feed(f2, two.data(), static_cast<u32>(two.size()));
    CHECK_EQ(g2.protocols.size(), 2u);
}
