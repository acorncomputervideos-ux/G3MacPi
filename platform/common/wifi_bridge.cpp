// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/wifi_bridge.cpp — see wifi_bridge.h.
//
// Frame layouts used here:
//   Ethernet II: destination 0-5, source 6-11, type 12-13 (>= 0x0600);
//     below 0x0600 it is an IEEE 802.3 length, and an 802.2 LLC/SNAP header
//     follows at 14: AA AA 03, OUI (3 bytes), type (2 bytes).
//   ARP over Ethernet for IPv4 (RFC 826), at 14: hardware type 1, protocol
//     0x0800, lengths 6 and 4, operation, sender hardware address (22),
//     sender protocol address (28), target hardware address (32), target
//     protocol address (38).
//   AppleTalk on Ethernet (Inside AppleTalk, 2nd ed., ch. 2 and 3): SNAP
//     OUI 08-00-07 type 0x809B for DDP, OUI 00-00-00 type 0x80F3 for AARP.
//     The AARP packet at 22: hardware type, protocol type, lengths 6 and 4,
//     function, source hardware address (30), source AppleTalk address (36),
//     destination hardware address (40), destination AppleTalk address (46).
//   IPv4 (RFC 791) at 14: header length in the low 4 bits of byte 14,
//     protocol at 23, source 26-29, destination 30-33. UDP (RFC 768) after
//     it: ports, length, checksum (6). DHCP (RFC 2131) after that: flags
//     at 10 (BROADCAST is the top bit), chaddr at 28.

#include "platform/common/wifi_bridge.h"

namespace g3 {

namespace {

bool same6(const u8* a, const u8* b)
{
    for (u32 i = 0; i < 6; ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}
void put6(u8* d, const u8* s)
{
    for (u32 i = 0; i < 6; ++i)
        d[i] = s[i];
}
u32 be16(const u8* p) { return static_cast<u32>(p[0]) << 8 | p[1]; }
u32 be32(const u8* p) { return be16(p) << 16 | be16(p + 2); }
bool group(const u8* address) { return (address[0] & 1) != 0; }  // broadcast or multicast

constexpr u32 kArpLen = 42, kAarpLen = 50;

bool is_arp(const u8* f, u32 len)
{
    return len >= kArpLen && be16(f + 12) == 0x0806 && be16(f + 14) == 1 && be16(f + 16) == 0x0800 &&
           f[18] == 6 && f[19] == 4;
}
bool snap(const u8* f, u32 len, u8 o0, u8 o1, u8 o2, u32 type)
{
    return len >= 22 && be16(f + 12) < 0x0600 && f[14] == 0xAA && f[15] == 0xAA && f[16] == 0x03 &&
           f[17] == o0 && f[18] == o1 && f[19] == o2 && be16(f + 20) == type;
}
bool is_ddp(const u8* f, u32 len) { return snap(f, len, 0x08, 0x00, 0x07, 0x809B); }
bool is_aarp(const u8* f, u32 len) { return len >= kAarpLen && snap(f, len, 0x00, 0x00, 0x00, 0x80F3); }
bool is_ipv4(const u8* f, u32 len)
{
    return len >= 34 && be16(f + 12) == 0x0800 && (f[14] >> 4) == 4 && (f[14] & 0xF) >= 5;
}
// The DHCP message in an IPv4 frame from `from_port` to `to_port`, or null.
u8* dhcp(u8* f, u32 len, u32 from_port, u32 to_port)
{
    if (!is_ipv4(f, len) || f[23] != 17)
        return nullptr;
    const u32 udp = 14 + (f[14] & 0xFu) * 4;
    if (len < udp + 8 + 34 || be16(f + udp) != from_port || be16(f + udp + 2) != to_port)
        return nullptr;
    return f + udp + 8;
}

} // namespace

WifiBridge::WifiBridge(const u8 host_address[6], const u8 mac_address[6])
{
    put6(host_, host_address);
    put6(mac_, mac_address);
}

u32 WifiBridge::route_from_mac(const u8* f, u32 len)
{
    if (len < 14)
        return 0;
    if (is_arp(f, len) && same6(f + 22, mac_) && be32(f + 28) != 0)
        mac_ip_ = be32(f + 28);
    else if (is_ipv4(f, len) && be32(f + 26) != 0)
        mac_ip_ = be32(f + 26);
    if (group(f))
        return kToAir | kToHost;
    if (same6(f, host_))
        return kToHost;
    if (same6(f, mac_))
        return 0;
    return kToAir;
}

void WifiBridge::to_air(u8* f, u32 len) const
{
    if (len < 14)
        return;
    put6(f + 6, host_);
    if (is_arp(f, len) && same6(f + 22, mac_))
        put6(f + 22, host_);
    if (is_aarp(f, len) && same6(f + 30, mac_))
        put6(f + 30, host_);
    if (u8* d = dhcp(f, len, 68, 67)) {
        d[10] |= 0x80;  // BROADCAST
        const u32 udp = 14 + (f[14] & 0xFu) * 4;
        f[udp + 6] = f[udp + 7] = 0;  // no checksum
    }
}

u32 WifiBridge::route_from_host(const u8* f, u32 len) const
{
    if (len < 14)
        return 0;
    if (group(f))
        return kToAir | kToMac;
    if (same6(f, mac_))
        return kToMac;
    if (same6(f, host_))
        return 0;
    return kToAir;
}

u32 WifiBridge::from_air(u8* f, u32 len)
{
    if (len < 14 || same6(f + 6, host_))
        return 0;  // too short, or the host's own (repeated by the access point)
    if (group(f))
        return kToMac | kToHost;
    if (same6(f, mac_))
        return kToMac;
    if (!same6(f, host_))
        return 0;
    bool to_mac = false;
    if (is_arp(f, len)) {
        to_mac = mac_ip_ != 0 && be32(f + 38) == mac_ip_;
        if (to_mac && same6(f + 32, host_))
            put6(f + 32, mac_);
    } else if (is_ipv4(f, len)) {
        const u8* d = dhcp(f, len, 67, 68);
        to_mac = (mac_ip_ != 0 && be32(f + 30) == mac_ip_) || (d != nullptr && same6(d + 28, mac_));
    } else if (is_ddp(f, len)) {
        to_mac = true;
    } else if (is_aarp(f, len)) {
        to_mac = true;
        if (same6(f + 40, host_))
            put6(f + 40, mac_);
    }
    if (!to_mac)
        return kToHost;
    put6(f, mac_);
    return kToMac;
}

} // namespace g3
