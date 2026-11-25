// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/dialup_link.cpp — see dialup_link.h.

#include "platform/common/dialup_link.h"

namespace g3 {

namespace {
u16 be16(const u8* p) { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | p[2] << 8 | p[3]; }
void put16(u8* p, u32 v)
{
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
void put32(u8* p, u32 v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}
void copy(u8* d, const u8* s, u32 n)
{
    for (u32 i = 0; i < n; ++i)
        d[i] = s[i];
}
bool same6(const u8* a, const u8* b)
{
    for (u32 i = 0; i < 6; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
const u8 kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const u8 kZero[6] = {};

// The IPv4 header checksum (RFC 791): one's complement of the one's
// complement sum of the header's 16-bit words.
u16 ip_checksum(const u8* h, u32 len)
{
    u32 sum = 0;
    for (u32 i = 0; i + 1 < len; i += 2)
        sum += be16(h + i);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}

enum : u8 { kDiscover = 1, kOffer = 2, kRequest = 3, kAck = 5, kNak = 6 };
} // namespace

DialupLink::DialupLink(const u8 mac_address[6]) { copy(mac_, mac_address, 6); }

void DialupLink::start(u64 now_us)
{
    state_ = State::Selecting;
    xid_ = 0x47330000u ^ static_cast<u32>(now_us);
    addr_ = PppServer::Addresses{};
    router_ = mask_ = 0;
    renew_at_ = 0;
    for (ArpEntry& e : cache_)
        e.valid = false;
    for (Held& h : held_)
        h.used = false;
    arp_for_ = 0;
    dhcp_send(kDiscover, now_us);
}

void DialupLink::stop() { state_ = State::Off; }

// --- frames out -------------------------------------------------------------------

void DialupLink::send_frame(const u8 dst[6], u16 type, const u8* payload, u32 len)
{
    static u8 f[14 + 1500];
    if (len > 1500 || !net_)
        return;
    copy(f, dst, 6);
    copy(f + 6, mac_, 6);
    put16(f + 12, type);
    copy(f + 14, payload, len);
    net_(net_ctx_, f, 14 + len);
}

void DialupLink::send_arp(u16 op, const u8 target_mac[6], u32 target_ip)
{
    u8 a[28];
    put16(a, 1);           // Ethernet
    put16(a + 2, 0x0800);  // IPv4
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    copy(a + 8, mac_, 6);
    put32(a + 14, addr_.client);
    copy(a + 18, target_mac, 6);
    put32(a + 24, target_ip);
    send_frame(op == 1 ? kBroadcast : target_mac, 0x0806, a, 28);
}

// --- DHCP -------------------------------------------------------------------------

void DialupLink::dhcp_send(u8 type, u64 now_us)
{
    static u8 p[20 + 8 + 300];
    for (u8& b : p)
        b = 0;
    const bool renewing = state_ == State::Bound;
    u8* ip = p;
    u8* udp = p + 20;
    u8* b = p + 28;
    // BOOTP
    b[0] = 1;  // BOOTREQUEST
    b[1] = 1;
    b[2] = 6;
    put32(b + 4, xid_);
    put16(b + 10, renewing ? 0 : 0x8000);  // BROADCAST: the answer cannot be unicast before we have an address
    if (renewing)
        put32(b + 12, addr_.client);       // ciaddr
    copy(b + 28, mac_, 6);                 // chaddr
    u32 o = 236;
    const u8 cookie[4] = {99, 130, 83, 99};
    copy(b + o, cookie, 4);
    o += 4;
    b[o++] = 53;
    b[o++] = 1;
    b[o++] = type;
    if (type == kRequest && !renewing) {
        b[o++] = 50;
        b[o++] = 4;
        put32(b + o, offered_);
        o += 4;
        b[o++] = 54;
        b[o++] = 4;
        put32(b + o, server_id_);
        o += 4;
    }
    b[o++] = 61;  // client identifier: hardware type and address
    b[o++] = 7;
    b[o++] = 1;
    copy(b + o, mac_, 6);
    o += 6;
    const u8 params[6] = {55, 4, 1, 3, 6, 51};
    copy(b + o, params, 6);
    o += 6;
    b[o++] = 255;
    while (o < 300)
        b[o++] = 0;  // BOOTP's minimum size (some servers want it)
    // UDP (no checksum: allowed over IPv4, RFC 768)
    put16(udp, 68);
    put16(udp + 2, 67);
    put16(udp + 4, 8 + o);
    // IPv4
    ip[0] = 0x45;
    put16(ip + 2, 20 + 8 + o);
    ip[8] = 64;
    ip[9] = 17;
    put32(ip + 12, renewing ? addr_.client : 0);
    put32(ip + 16, 0xFFFFFFFFu);
    put16(ip + 10, ip_checksum(ip, 20));
    send_frame(kBroadcast, 0x0800, p, 20 + 8 + o);
    dhcp_at_ = now_us + kDhcpRetryUs;
}

void DialupLink::dhcp_reply(const u8* b, u32 len, u64 now_us)
{
    if (len < 240 || b[0] != 2 || be32(b + 4) != xid_ || !same6(b + 28, mac_) || be32(b + 236) != 0x63825363u)
        return;
    u8 type = 0;
    u32 server = 0, mask = 0, router = 0, dns[2] = {}, lease = 0;
    for (u32 o = 240; o < len;) {
        const u8 t = b[o];
        if (t == 255)
            break;
        if (t == 0) {
            ++o;
            continue;
        }
        if (o + 2 > len || o + 2 + b[o + 1] > len)
            break;
        const u8 l = b[o + 1];
        const u8* v = b + o + 2;
        if (t == 53 && l == 1)
            type = v[0];
        else if (t == 54 && l == 4)
            server = be32(v);
        else if (t == 1 && l == 4)
            mask = be32(v);
        else if (t == 3 && l >= 4)
            router = be32(v);
        else if (t == 6 && l >= 4) {
            dns[0] = be32(v);
            if (l >= 8)
                dns[1] = be32(v + 4);
        } else if (t == 51 && l == 4)
            lease = be32(v);
        o += 2 + l;
    }
    const u32 yiaddr = be32(b + 16);
    if (type == kOffer && state_ == State::Selecting && yiaddr) {
        offered_ = yiaddr;
        server_id_ = server;
        state_ = State::Requesting;
        dhcp_send(kRequest, now_us);
        return;
    }
    if (type == kNak && state_ != State::Off) {
        state_ = State::Selecting;
        xid_ += 1;
        dhcp_send(kDiscover, now_us);
        return;
    }
    if (type != kAck || (state_ != State::Requesting && state_ != State::Bound) || !yiaddr)
        return;
    const bool first = state_ == State::Requesting;
    state_ = State::Bound;
    mask_ = mask ? mask : 0xFFFFFF00u;
    router_ = router;
    addr_.client = yiaddr;
    addr_.server = router ? router : server;
    addr_.dns1 = dns[0];
    addr_.dns2 = dns[1];
    renew_at_ = lease && lease != 0xFFFFFFFFu ? now_us + static_cast<u64>(lease) * 500000u : 0;  // half the lease
    if (first && ready_)
        ready_(ready_ctx_, addr_);
}

// --- ARP and routing ----------------------------------------------------------------

bool DialupLink::lookup(u32 ip, u8 out[6]) const
{
    for (const ArpEntry& e : cache_)
        if (e.valid && e.ip == ip) {
            copy(out, e.mac, 6);
            return true;
        }
    return false;
}

void DialupLink::learn(u32 ip, const u8 mac[6], u64 now_us)
{
    ArpEntry* slot = nullptr;
    for (ArpEntry& e : cache_)
        if (e.valid && e.ip == ip)
            slot = &e;
    if (!slot) {
        slot = &cache_[cache_next_];
        cache_next_ = (cache_next_ + 1) % kArpCache;
    }
    slot->ip = ip;
    copy(slot->mac, mac, 6);
    slot->valid = true;
    // Packets held for it go now.
    for (Held& h : held_)
        if (h.used && h.hop == ip) {
            send_frame(mac, 0x0800, h.data, h.len);
            h.used = false;
        }
    if (arp_for_ == ip) {
        arp_for_ = 0;
        for (const Held& h : held_)  // another next hop waiting: ask for it
            if (h.used) {
                arp_for_ = h.hop;
                arp_tries_ = 1;
                arp_at_ = now_us + kArpRetryUs;
                send_arp(1, kZero, h.hop);
                break;
            }
    }
}

void DialupLink::arp_frame(const u8* a, u64 now_us)
{
    if (be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4)
        return;
    const u16 op = be16(a + 6);
    const u32 spa = be32(a + 14), tpa = be32(a + 24);
    if (spa && spa != addr_.client)
        learn(spa, a + 8, now_us);
    if (op == 1 && tpa == addr_.client)
        send_arp(2, a + 8, spa);  // the Mac's address is the Mac's
}

void DialupLink::route(const u8* packet, u32 len, u64 now_us)
{
    const u32 dst = be32(packet + 16);
    if (dst == 0xFFFFFFFFu || dst == (addr_.client | ~mask_)) {
        send_frame(kBroadcast, 0x0800, packet, len);
        return;
    }
    const u32 hop = ((dst ^ addr_.client) & mask_) == 0 || !router_ ? dst : router_;
    u8 mac[6];
    if (lookup(hop, mac)) {
        send_frame(mac, 0x0800, packet, len);
        return;
    }
    Held* slot = nullptr;
    for (Held& h : held_)
        if (!h.used) {
            slot = &h;
            break;
        }
    if (!slot)
        return;  // too many waiting: dropped (TCP sends it again)
    slot->used = true;
    slot->hop = hop;
    slot->len = len;
    copy(slot->data, packet, len);
    if (!arp_for_) {
        arp_for_ = hop;
        arp_tries_ = 1;
        arp_at_ = now_us + kArpRetryUs;
        send_arp(1, kZero, hop);
    }
}

// --- in and out ---------------------------------------------------------------------

void DialupLink::from_mac(const u8* packet, u32 len, u64 now_us)
{
    if (state_ != State::Bound || len < 20 || len > 1500 || (packet[0] >> 4) != 4)
        return;
    route(packet, len, now_us);
}

void DialupLink::from_network(const u8* f, u32 len, u64 now_us)
{
    if (state_ == State::Off || len < 14)
        return;
    const u16 type = be16(f + 12);
    if (type == 0x0806 && len >= 14 + 28) {
        if (state_ == State::Bound)
            arp_frame(f + 14, now_us);
        return;
    }
    if (type != 0x0800 || len < 14 + 20)
        return;
    const u8* ip = f + 14;
    const u32 ihl = (ip[0] & 0xFu) * 4;
    u32 total = be16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > len - 14)
        return;
    if (ip[9] == 17 && total >= ihl + 8 && be16(ip + ihl) == 67 && be16(ip + ihl + 2) == 68) {
        dhcp_reply(ip + ihl + 8, total - ihl - 8, now_us);
        return;
    }
    if (state_ == State::Bound && be32(ip + 16) == addr_.client && same6(f, mac_) && to_mac_)
        to_mac_(to_mac_ctx_, ip, total);
}

void DialupLink::tick(u64 now_us)
{
    if (state_ == State::Off)
        return;
    if ((state_ == State::Selecting || state_ == State::Requesting) && now_us >= dhcp_at_)
        dhcp_send(state_ == State::Selecting ? kDiscover : kRequest, now_us);
    if (state_ == State::Bound && renew_at_ && now_us >= renew_at_) {
        dhcp_send(kRequest, now_us);
        renew_at_ = now_us + kDhcpRetryUs;  // until the ACK sets the next one
    }
    if (arp_for_ && now_us >= arp_at_) {
        if (arp_tries_ < kArpTries) {
            ++arp_tries_;
            arp_at_ = now_us + kArpRetryUs;
            send_arp(1, kZero, arp_for_);
        } else {
            for (Held& h : held_)  // no answer: what waited for it is dropped
                if (h.used && h.hop == arp_for_)
                    h.used = false;
            arp_for_ = 0;
            for (const Held& h : held_)
                if (h.used) {
                    arp_for_ = h.hop;
                    arp_tries_ = 1;
                    arp_at_ = now_us + kArpRetryUs;
                    send_arp(1, kZero, h.hop);
                    break;
                }
        }
    }
}

} // namespace g3
