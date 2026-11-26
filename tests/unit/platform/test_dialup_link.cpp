// SPDX-License-Identifier: GPL-3.0-or-later
// The dial-up provider's network side (platform/common/dialup_link.h):
// DHCP for the Mac, ARP, and IPv4 between the PPP link and Ethernet.

#include "platform/common/dialup_link.h"
#include "tests/framework/g3test.h"

#include <vector>

using namespace g3;

namespace {
constexpr u64 kSec = 1000000;
const u8 kMac[6] = {0x02, 0x47, 0x33, 0x4D, 0x50, 0x01};
const u8 kRouter[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
const u8 kHost[6] = {0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
constexpr u32 kClient = 0xC0A80132, kRouterIp = 0xC0A80101, kDns1 = 0x08080808, kDns2 = 0x01010101;

u16 g16(const std::vector<u8>& v, size_t o) { return static_cast<u16>(v[o] << 8 | v[o + 1]); }
u32 g32(const std::vector<u8>& v, size_t o) { return static_cast<u32>(g16(v, o)) << 16 | g16(v, o + 2); }
void p16(std::vector<u8>& v, size_t o, u32 x) { v[o] = static_cast<u8>(x >> 8); v[o + 1] = static_cast<u8>(x); }
void p32(std::vector<u8>& v, size_t o, u32 x) { p16(v, o, x >> 16); p16(v, o + 2, x); }

struct Net {
    DialupLink link{kMac};
    std::vector<std::vector<u8>> sent;
    std::vector<std::vector<u8>> to_mac;
    int ready = 0;
    PppServer::Addresses got;
    u64 now = 0;
    Net()
    {
        link.set_network([](void* c, const u8* f, u32 n) { static_cast<Net*>(c)->sent.emplace_back(f, f + n); }, this);
        link.set_mac_side([](void* c, const u8* p, u32 n) { static_cast<Net*>(c)->to_mac.emplace_back(p, p + n); }, this);
        link.set_ready([](void* c, const PppServer::Addresses& a) {
            auto* n = static_cast<Net*>(c);
            ++n->ready;
            n->got = a;
        }, this);
    }
    std::vector<u8> take()
    {
        CHECK(!sent.empty());
        if (sent.empty())
            return std::vector<u8>(600, 0);  // (checks on it fail, instead of reading past the end)
        std::vector<u8> f = sent.front();
        sent.erase(sent.begin());
        return f;
    }
    // A DHCP option's value in a frame the link sent (or empty).
    static std::vector<u8> option(const std::vector<u8>& f, u8 type)
    {
        for (size_t o = 14 + 20 + 8 + 240; o + 1 < f.size() && f[o] != 255;) {
            if (f[o] == 0) { ++o; continue; }
            if (f[o] == type)
                return std::vector<u8>(f.begin() + static_cast<long>(o) + 2, f.begin() + static_cast<long>(o) + 2 + f[o + 1]);
            o += 2 + f[o + 1];
        }
        return {};
    }
    void reply(u8 type, u32 xid, u32 yiaddr, bool full_options, const u8* chaddr = kMac)
    {
        std::vector<u8> b(240, 0);
        b[0] = 2; b[1] = 1; b[2] = 6;
        p32(b, 4, xid);
        p32(b, 16, yiaddr);
        for (int i = 0; i < 6; ++i) b[28 + i] = chaddr[i];
        p32(b, 236, 0x63825363);
        auto opt = [&](std::vector<u8> o) { b.insert(b.end(), o.begin(), o.end()); };
        opt({53, 1, type});
        opt({54, 4, 192, 168, 1, 1});
        if (full_options) {
            opt({1, 4, 255, 255, 255, 0});
            opt({3, 4, 192, 168, 1, 1});
            opt({6, 8, 8, 8, 8, 8, 1, 1, 1, 1});
            opt({51, 4, 0, 0, 0x0E, 0x10});  // 3600 s
        }
        opt({255});
        std::vector<u8> f(14 + 20 + 8, 0);
        for (int i = 0; i < 6; ++i) { f[i] = 0xFF; f[6 + i] = kRouter[i]; }
        p16(f, 12, 0x0800);
        f[14] = 0x45;
        p16(f, 16, static_cast<u32>(20 + 8 + b.size()));
        f[23] = 17;
        p32(f, 26, kRouterIp);
        p32(f, 30, 0xFFFFFFFF);
        p16(f, 34, 67);
        p16(f, 36, 68);
        p16(f, 38, static_cast<u32>(8 + b.size()));
        f.insert(f.end(), b.begin(), b.end());
        link.from_network(f.data(), static_cast<u32>(f.size()), now);
    }
    void arp(u16 op, const u8* sha, u32 spa, const u8* tha, u32 tpa, const u8* dst)
    {
        std::vector<u8> f(14 + 28, 0);
        for (int i = 0; i < 6; ++i) { f[i] = dst[i]; f[6 + i] = sha[i]; f[22 + i] = sha[i]; f[32 + i] = tha[i]; }
        p16(f, 12, 0x0806);
        p16(f, 14, 1);
        p16(f, 16, 0x0800);
        f[18] = 6; f[19] = 4;
        p16(f, 20, op);
        p32(f, 28, spa);
        p32(f, 38, tpa);
        link.from_network(f.data(), static_cast<u32>(f.size()), now);
    }
    void bind()
    {
        link.start(now);
        std::vector<u8> d = take();
        reply(2, g32(d, 14 + 28 + 4), kClient, true);
        std::vector<u8> r = take();
        reply(5, g32(r, 14 + 28 + 4), kClient, true);
    }
};

std::vector<u8> ip_packet(u32 src, u32 dst, u32 len = 28)
{
    std::vector<u8> p(len, 0);
    p[0] = 0x45;
    p16(p, 2, len);
    p[8] = 64;
    p[9] = 17;
    p32(p, 12, src);
    p32(p, 16, dst);
    for (u32 i = 20; i < len; ++i) p[i] = static_cast<u8>(i);
    return p;
}
} // namespace

TEST_CASE("platform/dialup_link/dhcp_for_the_mac")
{
    Net n;
    n.link.start(n.now);
    std::vector<u8> d = n.take();
    // DISCOVER: broadcast, from 0.0.0.0:68 to 255.255.255.255:67, as the Mac.
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(d[i], 0xFFu);
        CHECK_EQ(d[6 + i], kMac[i]);
    }
    CHECK_EQ(g16(d, 12), 0x0800u);
    CHECK_EQ(g32(d, 26), 0u);
    CHECK_EQ(g32(d, 30), 0xFFFFFFFFu);
    CHECK_EQ(g16(d, 34), 68u);
    CHECK_EQ(g16(d, 36), 67u);
    CHECK_EQ(d[42], 1u);                                  // BOOTREQUEST
    CHECK_EQ(g16(d, 42 + 10), 0x8000u);                   // BROADCAST
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(d[42 + 28 + i], kMac[i]);                // chaddr
    CHECK(Net::option(d, 53) == std::vector<u8>({1}));
    CHECK(Net::option(d, 55) == std::vector<u8>({1, 3, 6, 51}));
    // IPv4 header checksum: the header sums to 0xFFFF.
    u32 sum = 0;
    for (int i = 0; i < 20; i += 2) sum += g16(d, 14 + static_cast<size_t>(i));
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    CHECK_EQ(sum, 0xFFFFu);
    const u32 xid = g32(d, 42 + 4);
    // Answers for someone else are ignored.
    n.reply(2, xid + 1, kClient, true);
    n.reply(2, xid, kClient, true, kHost);
    CHECK(n.sent.empty());
    // OFFER -> REQUEST with the offered address and the server.
    n.reply(2, xid, kClient, true);
    std::vector<u8> r = n.take();
    CHECK(Net::option(r, 53) == std::vector<u8>({3}));
    CHECK(Net::option(r, 50) == std::vector<u8>({192, 168, 1, 50}));
    CHECK(Net::option(r, 54) == std::vector<u8>({192, 168, 1, 1}));
    CHECK(!n.link.ready());
    // ACK: bound; the PPP side gets the addresses (the router as its end).
    n.reply(5, xid, kClient, true);
    CHECK(n.link.ready());
    CHECK_EQ(n.ready, 1);
    CHECK_EQ(n.got.client, kClient);
    CHECK_EQ(n.got.server, kRouterIp);
    CHECK_EQ(n.got.dns1, kDns1);
    CHECK_EQ(n.got.dns2, kDns2);
    CHECK_EQ(n.link.netmask(), 0xFFFFFF00u);
    // Renewal at half the lease (1800 s): REQUEST from the Mac's address.
    n.now += 1799 * kSec;
    n.link.tick(n.now);
    CHECK(n.sent.empty());
    n.now += kSec;
    n.link.tick(n.now);
    std::vector<u8> renew = n.take();
    CHECK(Net::option(renew, 53) == std::vector<u8>({3}));
    CHECK_EQ(g32(renew, 26), kClient);                    // source address
    CHECK_EQ(g32(renew, 42 + 12), kClient);               // ciaddr
    CHECK_EQ(g16(renew, 42 + 10), 0u);
    n.reply(5, xid, kClient, true);
    CHECK_EQ(n.ready, 1);                                 // not announced twice
}

TEST_CASE("platform/dialup_link/dhcp_retries_and_nak")
{
    Net n;
    n.link.start(n.now);
    const u32 xid = g32(n.take(), 42 + 4);
    n.now += DialupLink::kDhcpRetryUs - 1;
    n.link.tick(n.now);
    CHECK(n.sent.empty());
    n.now += 1;
    n.link.tick(n.now);
    CHECK(Net::option(n.take(), 53) == std::vector<u8>({1}));
    n.reply(2, xid, kClient, true);
    n.take();
    n.reply(6, xid, 0, false);                            // NAK: start again
    std::vector<u8> d = n.take();
    CHECK(Net::option(d, 53) == std::vector<u8>({1}));
    CHECK(!n.link.ready());
}

TEST_CASE("platform/dialup_link/arp_and_routing")
{
    Net n;
    n.bind();
    n.sent.clear();
    // Who has the Mac's address? The Mac (its hardware address), to the asker.
    const u8 zero[6] = {};
    n.arp(1, kHost, 0xC0A8014D, zero, kClient, zero);
    std::vector<u8> rep = n.take();
    CHECK_EQ(g16(rep, 12), 0x0806u);
    CHECK_EQ(g16(rep, 20), 2u);
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(rep[i], kHost[i]);
        CHECK_EQ(rep[22 + i], kMac[i]);
    }
    CHECK_EQ(g32(rep, 28), kClient);
    // (That taught it the host's address.) Off the subnet: via the router,
    // which is not known yet: an ARP request, the packet held.
    const std::vector<u8> out = ip_packet(kClient, 0x5DB8D822);
    n.link.from_mac(out.data(), static_cast<u32>(out.size()), n.now);
    std::vector<u8> req = n.take();
    CHECK_EQ(g16(req, 12), 0x0806u);
    CHECK_EQ(g16(req, 20), 1u);
    CHECK_EQ(g32(req, 38), kRouterIp);
    CHECK(n.sent.empty());
    n.arp(2, kRouter, kRouterIp, kMac, kClient, kMac);
    std::vector<u8> f = n.take();
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(f[i], kRouter[i]);
    CHECK(std::vector<u8>(f.begin() + 14, f.end()) == out);
    // Now straight away.
    n.link.from_mac(out.data(), static_cast<u32>(out.size()), n.now);
    CHECK_EQ(n.take()[0], kRouter[0]);
    // On the subnet: directly to the host (learnt from its ARP).
    const std::vector<u8> local = ip_packet(kClient, 0xC0A8014D);
    n.link.from_mac(local.data(), static_cast<u32>(local.size()), n.now);
    CHECK_EQ(n.take()[1], kHost[1]);
    // Broadcast.
    const std::vector<u8> bc = ip_packet(kClient, 0xC0A801FF);
    n.link.from_mac(bc.data(), static_cast<u32>(bc.size()), n.now);
    CHECK_EQ(n.take()[0], 0xFFu);
}

TEST_CASE("platform/dialup_link/arp_gives_up")
{
    Net n;
    n.bind();
    n.sent.clear();
    const std::vector<u8> out = ip_packet(kClient, 0xC0A80199);
    n.link.from_mac(out.data(), static_cast<u32>(out.size()), n.now);
    CHECK_EQ(g16(n.take(), 20), 1u);
    for (u32 i = 1; i < DialupLink::kArpTries; ++i) {
        n.now += DialupLink::kArpRetryUs;
        n.link.tick(n.now);
        CHECK_EQ(g16(n.take(), 20), 1u);
    }
    n.now += DialupLink::kArpRetryUs;
    n.link.tick(n.now);
    CHECK(n.sent.empty());
    // A late answer sends nothing (it was dropped).
    const u8 other[6] = {0x00, 1, 2, 3, 4, 5};
    n.arp(2, other, 0xC0A80199, kMac, kClient, kMac);
    CHECK(n.sent.empty());
}

TEST_CASE("platform/dialup_link/to_the_mac")
{
    Net n;
    // Before the call: nothing.
    std::vector<u8> in = ip_packet(0x5DB8D822, kClient);
    std::vector<u8> f(14, 0);
    for (int i = 0; i < 6; ++i) { f[i] = kMac[i]; f[6 + i] = kRouter[i]; }
    p16(f, 12, 0x0800);
    f.insert(f.end(), in.begin(), in.end());
    f.resize(f.size() + 18, 0);                           // Ethernet padding after the packet
    n.link.from_network(f.data(), static_cast<u32>(f.size()), n.now);
    CHECK(n.to_mac.empty());
    n.bind();
    n.link.from_network(f.data(), static_cast<u32>(f.size()), n.now);
    REQUIRE(n.to_mac.size() == 1u);
    CHECK(n.to_mac[0] == in);                             // the packet, without the padding
    // Another address, or another hardware address: not for the Mac.
    std::vector<u8> g = f;
    p32(g, 14 + 16, 0xC0A80133);
    n.link.from_network(g.data(), static_cast<u32>(g.size()), n.now);
    std::vector<u8> h = f;
    h[0] ^= 1;
    n.link.from_network(h.data(), static_cast<u32>(h.size()), n.now);
    CHECK_EQ(n.to_mac.size(), 1u);
    // After the call: nothing.
    n.link.stop();
    n.link.from_network(f.data(), static_cast<u32>(f.size()), n.now);
    CHECK_EQ(n.to_mac.size(), 1u);
}
