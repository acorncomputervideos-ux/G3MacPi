// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_wifi_bridge.cpp — the Mac on the Pi's WiFi
// (platform/common/wifi_bridge): the Mac's frames leave under the Pi's
// address, and answers come back to whichever of the two they are for.

#include "platform/common/wifi_bridge.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

namespace {

const u8 kPi[6] = {0xDC, 0xA6, 0x32, 0x01, 0x02, 0x03};
const u8 kMac[6] = {0xDE, 0xA6, 0x32, 0x01, 0x02, 0x03};
const u8 kRouter[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
const u8 kAll[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const u8 kAtBroadcast[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};

using Frame = std::vector<u8>;

void put6(Frame& f, u32 at, const u8* a) { std::memcpy(f.data() + at, a, 6); }
void put16(Frame& f, u32 at, u32 v) { f[at] = static_cast<u8>(v >> 8); f[at + 1] = static_cast<u8>(v); }
void put32(Frame& f, u32 at, u32 v) { put16(f, at, v >> 16); put16(f, at + 2, v & 0xFFFF); }
bool is6(const Frame& f, u32 at, const u8* a) { return std::memcmp(f.data() + at, a, 6) == 0; }

Frame eth(const u8* dst, const u8* src, u32 type, u32 len)
{
    Frame f(len, 0);
    put6(f, 0, dst);
    put6(f, 6, src);
    put16(f, 12, type);
    return f;
}

// ARP: op 1 request, 2 reply.
Frame arp(const u8* dst, const u8* src, u32 op, const u8* sha, u32 spa, const u8* tha, u32 tpa)
{
    Frame f = eth(dst, src, 0x0806, 60);
    put16(f, 14, 1);
    put16(f, 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    put16(f, 20, op);
    put6(f, 22, sha);
    put32(f, 28, spa);
    put6(f, 32, tha);
    put32(f, 38, tpa);
    return f;
}

Frame ipv4(const u8* dst, const u8* src, u32 sip, u32 dip, u8 proto = 6, u32 len = 60)
{
    Frame f = eth(dst, src, 0x0800, len);
    f[14] = 0x45;
    f[23] = proto;
    put32(f, 26, sip);
    put32(f, 30, dip);
    return f;
}

// DHCP over UDP: 20-byte IP header, UDP at 34, BOOTP at 42.
Frame dhcp(const u8* dst, const u8* src, u32 sip, u32 dip, u32 sport, u32 dport, const u8* chaddr)
{
    Frame f = ipv4(dst, src, sip, dip, 17, 42 + 240);
    put16(f, 34, sport);
    put16(f, 36, dport);
    put16(f, 40, 0xBEEF);  // a checksum
    f[42] = sport == 68 ? 1 : 2;
    put6(f, 42 + 28, chaddr);
    return f;
}

// AppleTalk over 802.2 SNAP.
Frame snap(const u8* dst, const u8* src, const u8 oui[3], u32 type, u32 len)
{
    Frame f = eth(dst, src, len - 14, len);
    f[14] = f[15] = 0xAA;
    f[16] = 0x03;
    f[17] = oui[0];
    f[18] = oui[1];
    f[19] = oui[2];
    put16(f, 20, type);
    return f;
}
const u8 kApple[3] = {0x08, 0x00, 0x07}, kZero[6] = {0, 0, 0, 0, 0, 0};  // an OUI (3 bytes) or a MAC address (6)
Frame aarp(const u8* dst, const u8* src, u32 fn, const u8* sha, const u8* tha)
{
    Frame f = snap(dst, src, kZero, 0x80F3, 60);
    put16(f, 22, 1);
    put16(f, 24, 0x809B);
    f[26] = 6;
    f[27] = 4;
    put16(f, 28, fn);
    put6(f, 30, sha);
    put32(f, 36, 0x00FF0A01);
    put6(f, 40, tha);
    put32(f, 46, 0x00FF0A02);
    return f;
}

constexpr u32 kMacIp = 0xC0A80132, kPiIp = 0xC0A80131, kRouterIp = 0xC0A80101;  // 192.168.1.x

} // namespace

TEST_CASE("platform/wifibridge/mac_frames_leave_as_the_pi")
{
    WifiBridge b(kPi, kMac);
    CHECK_EQ(b.mac_ip(), 0u);
    // The Mac asks for the router: broadcast, to the air and to the Pi.
    Frame f = arp(kAll, kMac, 1, kMac, kMacIp, kZero, kRouterIp);
    CHECK_EQ(b.route_from_mac(f.data(), static_cast<u32>(f.size())), WifiBridge::kToAir | WifiBridge::kToHost);
    CHECK_EQ(b.mac_ip(), kMacIp);  // learnt
    b.to_air(f.data(), static_cast<u32>(f.size()));
    CHECK(is6(f, 6, kPi));   // source
    CHECK(is6(f, 22, kPi));  // ARP sender hardware address
    CHECK(is6(f, 0, kAll));
    // IPv4 to the router: to the air only.
    Frame g = ipv4(kRouter, kMac, kMacIp, 0x08080808);
    CHECK_EQ(b.route_from_mac(g.data(), static_cast<u32>(g.size())), WifiBridge::kToAir);
    b.to_air(g.data(), static_cast<u32>(g.size()));
    CHECK(is6(g, 6, kPi));
    CHECK(is6(g, 0, kRouter));
    // To the Pi: delivered directly, not to the air; to itself: nowhere.
    Frame h = ipv4(kPi, kMac, kMacIp, kPiIp);
    CHECK_EQ(b.route_from_mac(h.data(), static_cast<u32>(h.size())), WifiBridge::kToHost);
    Frame k = ipv4(kMac, kMac, kMacIp, kMacIp);
    CHECK_EQ(b.route_from_mac(k.data(), static_cast<u32>(k.size())), 0u);
    // Too short: nowhere, and to_air leaves it alone.
    u8 tiny[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3, 4};
    CHECK_EQ(b.route_from_mac(tiny, 10), 0u);
    b.to_air(tiny, 10);
    CHECK_EQ(tiny[6], 1);
}

TEST_CASE("platform/wifibridge/dhcp_asks_for_broadcast_answers")
{
    WifiBridge b(kPi, kMac);
    Frame d = dhcp(kAll, kMac, 0, 0xFFFFFFFF, 68, 67, kMac);  // DISCOVER from 0.0.0.0
    CHECK_EQ(b.route_from_mac(d.data(), static_cast<u32>(d.size())), WifiBridge::kToAir | WifiBridge::kToHost);
    CHECK_EQ(b.mac_ip(), 0u);  // 0.0.0.0 is not learnt
    b.to_air(d.data(), static_cast<u32>(d.size()));
    CHECK_EQ(d[42 + 10] & 0x80, 0x80);  // BROADCAST
    CHECK_EQ(d[40], 0);                 // checksum cleared
    CHECK_EQ(d[41], 0);
    CHECK(is6(d, 42 + 28, kMac));       // chaddr stays the Mac's
    // A server that answers by unicast to the Pi's address all the same:
    // the reply is for the Mac's chaddr, so the Mac gets it.
    Frame o = dhcp(kPi, kRouter, kRouterIp, kMacIp, 67, 68, kMac);
    CHECK_EQ(b.from_air(o.data(), static_cast<u32>(o.size())), WifiBridge::kToMac);
    CHECK(is6(o, 0, kMac));
    // The Pi's own DHCP reply stays with the Pi.
    Frame p = dhcp(kPi, kRouter, kRouterIp, kPiIp, 67, 68, kPi);
    CHECK_EQ(b.from_air(p.data(), static_cast<u32>(p.size())), WifiBridge::kToHost);
    CHECK(is6(p, 0, kPi));
    // Other UDP (not DHCP) from the Mac: unchanged checksum.
    Frame u = dhcp(kRouter, kMac, kMacIp, kRouterIp, 5000, 53, kMac);
    b.to_air(u.data(), static_cast<u32>(u.size()));
    CHECK_EQ(u[40], 0xBE);
    CHECK_EQ(u[42 + 10], 0);
}

TEST_CASE("platform/wifibridge/answers_sorted_by_ip")
{
    WifiBridge b(kPi, kMac);
    Frame learn = ipv4(kRouter, kMac, kMacIp, kRouterIp);
    b.route_from_mac(learn.data(), static_cast<u32>(learn.size()));
    CHECK_EQ(b.mac_ip(), kMacIp);
    // IPv4 for the Mac's address arrives at the Pi's: the Mac gets it.
    Frame f = ipv4(kPi, kRouter, kRouterIp, kMacIp);
    CHECK_EQ(b.from_air(f.data(), static_cast<u32>(f.size())), WifiBridge::kToMac);
    CHECK(is6(f, 0, kMac));
    CHECK(is6(f, 6, kRouter));
    // For the Pi's address: the Pi.
    Frame g = ipv4(kPi, kRouter, kRouterIp, kPiIp);
    CHECK_EQ(b.from_air(g.data(), static_cast<u32>(g.size())), WifiBridge::kToHost);
    CHECK(is6(g, 0, kPi));
    // ARP reply to the Mac's question: to the Mac, target changed.
    Frame a = arp(kPi, kRouter, 2, kRouter, kRouterIp, kPi, kMacIp);
    CHECK_EQ(b.from_air(a.data(), static_cast<u32>(a.size())), WifiBridge::kToMac);
    CHECK(is6(a, 0, kMac));
    CHECK(is6(a, 32, kMac));
    // ARP reply for the Pi: the Pi, unchanged.
    Frame c = arp(kPi, kRouter, 2, kRouter, kRouterIp, kPi, kPiIp);
    CHECK_EQ(b.from_air(c.data(), static_cast<u32>(c.size())), WifiBridge::kToHost);
    CHECK(is6(c, 32, kPi));
    // Broadcasts: both, unchanged. Others' unicast: nobody.
    Frame w = arp(kAll, kRouter, 1, kRouter, kRouterIp, kZero, kMacIp);
    CHECK_EQ(b.from_air(w.data(), static_cast<u32>(w.size())), WifiBridge::kToMac | WifiBridge::kToHost);
    CHECK(is6(w, 0, kAll));
    Frame x = ipv4(kRouter, kRouter, kRouterIp, kRouterIp);
    CHECK_EQ(b.from_air(x.data(), static_cast<u32>(x.size())), 0u);
    // The Pi's own broadcast repeated by the access point: dropped (else the
    // Mac would see its own address claimed by "another" station).
    Frame e = arp(kAll, kPi, 1, kPi, kMacIp, kZero, kMacIp);
    CHECK_EQ(b.from_air(e.data(), static_cast<u32>(e.size())), 0u);
    // A frame to the Mac's own address (not expected on WiFi): the Mac.
    Frame m = ipv4(kMac, kRouter, kRouterIp, kMacIp);
    CHECK_EQ(b.from_air(m.data(), static_cast<u32>(m.size())), WifiBridge::kToMac);
    // Before the Mac's address is known, IPv4 and ARP stay with the Pi.
    WifiBridge fresh(kPi, kMac);
    Frame n = ipv4(kPi, kRouter, kRouterIp, kMacIp);
    CHECK_EQ(fresh.from_air(n.data(), static_cast<u32>(n.size())), WifiBridge::kToHost);
}

TEST_CASE("platform/wifibridge/appletalk")
{
    WifiBridge b(kPi, kMac);
    // AARP probe from the Mac (AppleTalk broadcast): source addresses become the Pi's.
    Frame p = aarp(kAtBroadcast, kMac, 3, kMac, kZero);
    CHECK_EQ(b.route_from_mac(p.data(), static_cast<u32>(p.size())), WifiBridge::kToAir | WifiBridge::kToHost);
    b.to_air(p.data(), static_cast<u32>(p.size()));
    CHECK(is6(p, 6, kPi));
    CHECK(is6(p, 30, kPi));
    // An AARP response to the Pi's address: the Mac, destination changed.
    Frame r = aarp(kPi, kRouter, 2, kRouter, kPi);
    CHECK_EQ(b.from_air(r.data(), static_cast<u32>(r.size())), WifiBridge::kToMac);
    CHECK(is6(r, 0, kMac));
    CHECK(is6(r, 40, kMac));
    // DDP to the Pi's address: the Mac (the Pi does not speak AppleTalk).
    Frame d = snap(kPi, kRouter, kApple, 0x809B, 80);
    CHECK_EQ(b.from_air(d.data(), static_cast<u32>(d.size())), WifiBridge::kToMac);
    CHECK(is6(d, 0, kMac));
    // Other 802.2 frames to the Pi: the Pi.
    Frame o = snap(kPi, kRouter, kZero, 0x1234, 80);
    CHECK_EQ(b.from_air(o.data(), static_cast<u32>(o.size())), WifiBridge::kToHost);
}

TEST_CASE("platform/wifibridge/host_frames")
{
    WifiBridge b(kPi, kMac);
    Frame f = ipv4(kRouter, kPi, kPiIp, kRouterIp);
    CHECK_EQ(b.route_from_host(f.data(), static_cast<u32>(f.size())), WifiBridge::kToAir);
    Frame g = ipv4(kMac, kPi, kPiIp, kMacIp);
    CHECK_EQ(b.route_from_host(g.data(), static_cast<u32>(g.size())), WifiBridge::kToMac);
    Frame h = arp(kAll, kPi, 1, kPi, kPiIp, kZero, kMacIp);
    CHECK_EQ(b.route_from_host(h.data(), static_cast<u32>(h.size())), WifiBridge::kToAir | WifiBridge::kToMac);
    Frame s = ipv4(kPi, kPi, kPiIp, kPiIp);
    CHECK_EQ(b.route_from_host(s.data(), static_cast<u32>(s.size())), 0u);
    u8 tiny[8] = {};
    CHECK_EQ(b.route_from_host(tiny, 8), 0u);
}
