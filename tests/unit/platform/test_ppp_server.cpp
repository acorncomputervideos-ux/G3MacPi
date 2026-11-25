// SPDX-License-Identifier: GPL-3.0-or-later
// The dial-up provider's PPP end (platform/common/ppp_server.h): LCP, PAP,
// IPCP and IP, with the test playing the Mac's Remote Access.

#include "platform/common/ppp_server.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;

namespace {
constexpr u64 kSec = 1000000;

struct Packet {
    u16 protocol;
    std::vector<u8> info;
    u8 code() const { return info.size() > 0 ? info[0] : 0; }
    u8 id() const { return info.size() > 1 ? info[1] : 0; }
    std::vector<u8> data() const { return info.size() > 4 ? std::vector<u8>(info.begin() + 4, info.end()) : std::vector<u8>(); }
};

struct Client {
    PppServer s;
    PppFramer rx;                  // what the server sends, as the Mac decodes it
    std::vector<Packet> got;
    std::vector<u8> raw;           // the server's bytes
    std::vector<std::vector<u8>> ip_in;
    int hang_ups = 0;
    u64 now = 0;
    PppFramer::Options tx;         // how the "Mac" sends

    Client()
    {
        s.set_output([](void* c, const u8* d, u32 n) {
            auto* cl = static_cast<Client*>(c);
            cl->raw.insert(cl->raw.end(), d, d + n);
            for (u32 i = 0; i < n; ++i)
                cl->rx.receive(d[i]);
        }, this);
        rx.set_receiver([](void* c, u16 p, const u8* d, u32 n) {
            static_cast<Client*>(c)->got.push_back(Packet{p, std::vector<u8>(d, d + n)});
        }, this);
        s.set_ip_receiver([](void* c, const u8* d, u32 n) { static_cast<Client*>(c)->ip_in.emplace_back(d, d + n); }, this);
        s.set_hang_up([](void* c) { ++static_cast<Client*>(c)->hang_ups; }, this);
    }
    void send(u16 protocol, const std::vector<u8>& info)
    {
        u8 buf[4000];
        const u32 n = PppFramer::encode(protocol, info.data(), static_cast<u32>(info.size()), tx, buf);
        for (u32 i = 0; i < n; ++i)
            s.receive(buf[i], now);
    }
    void send_cp(u16 protocol, u8 code, u8 id, const std::vector<u8>& data)
    {
        std::vector<u8> p = {code, id, static_cast<u8>((data.size() + 4) >> 8), static_cast<u8>(data.size() + 4)};
        p.insert(p.end(), data.begin(), data.end());
        send(protocol, p);
    }
    Packet take()
    {
        CHECK(!got.empty());
        if (got.empty())
            return Packet{0, {}};
        Packet p = got.front();
        got.erase(got.begin());
        return p;
    }
    // Options of a configure packet, as type -> value bytes.
    static bool has(const std::vector<u8>& opts, u8 type, std::vector<u8>* value = nullptr)
    {
        for (size_t i = 0; i + 1 < opts.size() && opts[i + 1] >= 2; i += opts[i + 1])
            if (opts[i] == type) {
                if (value)
                    *value = std::vector<u8>(opts.begin() + static_cast<long>(i) + 2,
                                             opts.begin() + static_cast<long>(i) + opts[i + 1]);
                return true;
            }
        return false;
    }
    // LCP up the usual way (the Mac asks for MRU, ACCM 0, magic, PFC, ACFC)
    // and PAP done.
    void open_link()
    {
        s.start(now);
        Packet req = take();
        send_cp(0xC021, 1, 7, {1, 4, 0x05, 0xDC, 2, 6, 0, 0, 0, 0, 5, 6, 0x12, 0x34, 0x56, 0x78, 7, 2, 8, 2});
        Packet ack = take();
        REQUIRE(ack.code() == 2);
        send_cp(0xC021, 2, req.id(), req.data());
        tx.accm = 0;
        tx.acfc = tx.pfc = true;
        send_cp(0xC023, 1, 3, {2, 'm', 'e', 2, 'p', 'w'});
        Packet pap = take();
        CHECK_EQ(pap.protocol, 0xC023u);
        CHECK_EQ(pap.code(), 2u);
    }
};

std::vector<u8> addr_opt(u8 type, u32 v)
{
    return {type, 6, static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
}
} // namespace

TEST_CASE("platform/ppp_server/full_negotiation_and_ip")
{
    Client c;
    c.s.start(c.now);
    // The server's LCP request: ACCM 0, PAP, a magic number, PFC, ACFC.
    Packet req = c.take();
    CHECK_EQ(req.protocol, 0xC021u);
    CHECK_EQ(req.code(), 1u);
    std::vector<u8> v;
    REQUIRE(Client::has(req.data(), 3, &v));
    CHECK(v == std::vector<u8>({0xC0, 0x23}));
    CHECK(Client::has(req.data(), 2));
    CHECK(Client::has(req.data(), 5));
    CHECK(Client::has(req.data(), 7));
    CHECK(Client::has(req.data(), 8));
    // Before LCP is open: every control character escaped, full header.
    CHECK_EQ(c.raw[1], 0xFFu);
    // The Mac's request is acknowledged as it is.
    const std::vector<u8> mac_opts = {1, 4, 0x05, 0xDC, 2, 6, 0, 0, 0, 0, 5, 6, 0x12, 0x34, 0x56, 0x78, 7, 2, 8, 2};
    c.send_cp(0xC021, 1, 7, mac_opts);
    Packet ack = c.take();
    CHECK_EQ(ack.code(), 2u);
    CHECK_EQ(ack.id(), 7u);
    CHECK(ack.data() == mac_opts);
    CHECK(!c.s.lcp_open());
    c.send_cp(0xC021, 2, req.id(), req.data());
    CHECK(c.s.lcp_open());
    CHECK(!c.s.authenticated());
    c.tx.accm = 0;
    c.tx.acfc = c.tx.pfc = true;
    // PAP: anything goes.
    c.send_cp(0xC023, 1, 3, {2, 'm', 'e', 2, 'p', 'w'});
    Packet pap = c.take();
    CHECK_EQ(pap.protocol, 0xC023u);
    CHECK_EQ(pap.code(), 2u);
    CHECK(c.s.authenticated());
    // IPCP waits for the addresses: the Mac's request gets no answer yet.
    c.send_cp(0x8021, 1, 4, addr_opt(3, 0));
    CHECK(c.got.empty());
    PppServer::Addresses a;
    a.client = 0xC0A80132;  // 192.168.1.50
    a.server = 0xC0A80101;
    a.dns1 = 0x08080808;
    a.dns2 = 0x01010101;
    c.s.set_addresses(a, c.now);
    Packet ireq = c.take();
    CHECK_EQ(ireq.protocol, 0x8021u);
    CHECK(ireq.data() == addr_opt(3, a.server));
    // VJ compression and NBNS are rejected, nothing else in that answer.
    std::vector<u8> opts = addr_opt(3, 0);
    const std::vector<u8> vj = {2, 6, 0x00, 0x2D, 0x0F, 0x01};
    opts.insert(opts.end(), vj.begin(), vj.end());
    std::vector<u8> nbns = addr_opt(130, 0);
    opts.insert(opts.end(), nbns.begin(), nbns.end());
    c.send_cp(0x8021, 1, 5, opts);
    Packet rej = c.take();
    CHECK_EQ(rej.code(), 4u);
    std::vector<u8> both = vj;
    both.insert(both.end(), nbns.begin(), nbns.end());
    CHECK(rej.data() == both);
    // Address 0 and DNS 0: Nak with ours.
    opts = addr_opt(3, 0);
    for (u8 t : {129, 131}) {
        std::vector<u8> d = addr_opt(t, 0);
        opts.insert(opts.end(), d.begin(), d.end());
    }
    c.send_cp(0x8021, 1, 6, opts);
    Packet nak = c.take();
    CHECK_EQ(nak.code(), 3u);
    std::vector<u8> want = addr_opt(3, a.client);
    for (auto [t, x] : {std::pair<u8, u32>{129, a.dns1}, {131, a.dns2}}) {
        std::vector<u8> d = addr_opt(t, x);
        want.insert(want.end(), d.begin(), d.end());
    }
    CHECK(nak.data() == want);
    c.send_cp(0x8021, 1, 7, want);
    Packet iack = c.take();
    CHECK_EQ(iack.code(), 2u);
    CHECK(!c.s.ip_up());
    c.send_cp(0x8021, 2, ireq.id(), ireq.data());
    CHECK(c.s.ip_up());
    // IP both ways; to the Mac with the agreed framing (ACFC, PFC, ACCM 0).
    const std::vector<u8> packet = {0x45, 0, 0, 20, 0x11, 0x13, 0x7E};
    c.send(0x0021, packet);
    REQUIRE(c.ip_in.size() == 1u);
    CHECK(c.ip_in[0] == packet);
    c.raw.clear();
    CHECK(c.s.send_ip(packet.data(), static_cast<u32>(packet.size())));
    CHECK_EQ(c.raw[1], 0x21u);                      // one-byte protocol, no FF 03
    CHECK_EQ(c.raw[6], 0x11u);                      // XON raw: ACCM 0
    Packet ip = c.take();
    CHECK_EQ(ip.protocol, 0x0021u);
    CHECK(ip.info == packet);
    // Echo: answered with the server's magic number.
    c.send_cp(0xC021, 9, 40, {0x12, 0x34, 0x56, 0x78, 'h', 'i'});
    Packet echo = c.take();
    CHECK_EQ(echo.code(), 10u);
    CHECK_EQ(echo.id(), 40u);
    std::vector<u8> magic;
    REQUIRE(Client::has(req.data(), 5, &magic));
    std::vector<u8> e = magic;
    e.push_back('h');
    e.push_back('i');
    CHECK(echo.data() == e);
    // A protocol we do not speak (ATCP): Protocol-Reject naming it.
    c.send(0x8029, {1, 1, 0, 4});
    Packet pr = c.take();
    CHECK_EQ(pr.protocol, 0xC021u);
    CHECK_EQ(pr.code(), 8u);
    CHECK_EQ(pr.data()[0], 0x80u);
    CHECK_EQ(pr.data()[1], 0x29u);
    // An unknown LCP code: Code-Reject.
    c.send_cp(0xC021, 99, 1, {});
    CHECK_EQ(c.take().code(), 7u);
    // The Mac hangs up.
    c.send_cp(0xC021, 5, 41, {});
    Packet ta = c.take();
    CHECK_EQ(ta.code(), 6u);
    CHECK_EQ(ta.id(), 41u);
    CHECK_EQ(c.hang_ups, 1);
    CHECK(!c.s.running());
}

TEST_CASE("platform/ppp_server/retries_then_gives_up")
{
    Client c;
    c.s.start(c.now);
    CHECK_EQ(c.take().code(), 1u);
    c.now += PppServer::kRestartUs - 1;
    c.s.tick(c.now);
    CHECK(c.got.empty());
    c.now += 1;
    c.s.tick(c.now);
    Packet again = c.take();
    CHECK_EQ(again.code(), 1u);
    for (u32 i = 2; i < PppServer::kMaxConfigure; ++i) {
        c.now += PppServer::kRestartUs;
        c.s.tick(c.now);
        CHECK_EQ(c.take().code(), 1u);
    }
    CHECK_EQ(c.hang_ups, 0);
    c.now += PppServer::kRestartUs;
    c.s.tick(c.now);
    CHECK_EQ(c.hang_ups, 1);                         // ten requests unanswered
    CHECK(c.got.empty());
}

TEST_CASE("platform/ppp_server/no_authentication_if_refused")
{
    Client c;
    c.s.start(c.now);
    Packet req = c.take();
    // The Mac rejects PAP: asked again without it.
    c.send_cp(0xC021, 4, req.id(), {3, 4, 0xC0, 0x23});
    Packet req2 = c.take();
    CHECK_EQ(req2.code(), 1u);
    CHECK(!Client::has(req2.data(), 3));
    PppServer::Addresses a;
    a.client = 0x0A000002;
    a.server = 0x0A000001;
    c.s.set_addresses(a, c.now);
    CHECK(c.got.empty());                            // not before LCP is open
    c.send_cp(0xC021, 1, 9, {7, 2, 8, 2});
    CHECK_EQ(c.take().code(), 2u);
    c.send_cp(0xC021, 2, req2.id(), req2.data());
    CHECK(c.s.lcp_open());
    CHECK(c.s.authenticated());
    Packet ireq = c.take();                          // straight to IPCP
    CHECK_EQ(ireq.protocol, 0x8021u);
    // The Mac asks for nothing: told to take its address.
    c.tx.acfc = c.tx.pfc = true;
    c.send_cp(0x8021, 1, 2, {});
    Packet nak = c.take();
    CHECK_EQ(nak.code(), 3u);
    CHECK(nak.data() == addr_opt(3, a.client));
    // DNS asked for while none is known: rejected.
    c.send_cp(0x8021, 1, 3, addr_opt(129, 0));
    CHECK_EQ(c.take().code(), 4u);
}

TEST_CASE("platform/ppp_server/nothing_but_lcp_before_open")
{
    Client c;
    c.s.start(c.now);
    c.take();
    c.send(0x0021, {0x45, 0, 0, 20});
    c.send_cp(0xC023, 1, 1, {0, 0});
    c.send_cp(0x8021, 1, 1, addr_opt(3, 0));
    c.send(0x8029, {1, 1, 0, 4});
    CHECK(c.got.empty());
    CHECK(c.ip_in.empty());
    CHECK(!c.s.send_ip(reinterpret_cast<const u8*>("x"), 1));
}

TEST_CASE("platform/ppp_server/unsupported_options_rejected")
{
    Client c;
    c.s.start(c.now);
    c.take();
    // The Mac asks us to authenticate to it (option 3) and for call-back (13):
    // both rejected, and only those.
    const std::vector<u8> auth = {3, 5, 0xC2, 0x23, 0x05};
    const std::vector<u8> callback = {13, 3, 6};
    std::vector<u8> opts = {7, 2};
    opts.insert(opts.end(), auth.begin(), auth.end());
    opts.insert(opts.end(), callback.begin(), callback.end());
    c.send_cp(0xC021, 1, 3, opts);
    Packet rej = c.take();
    CHECK_EQ(rej.code(), 4u);
    std::vector<u8> both = auth;
    both.insert(both.end(), callback.begin(), callback.end());
    CHECK(rej.data() == both);
    CHECK(!c.s.lcp_open());
}

TEST_CASE("platform/ppp_server/no_ip_until_ipcp_is_open")
{
    Client c;
    c.open_link();
    CHECK(c.s.authenticated());
    c.send(0x0021, {0x45, 0, 0, 20});
    CHECK(c.ip_in.empty());                          // no addresses, no IPCP yet
    CHECK(!c.s.send_ip(reinterpret_cast<const u8*>("x"), 1));
}
