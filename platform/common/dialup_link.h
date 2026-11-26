// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/dialup_link.h — the dial-up provider's side towards the
// network the Pi is on (docs/devices/modem.md). The Mac's PPP link carries
// IPv4 packets; this puts them on the Ethernet (or WiFi, through the same
// path as the emulated Ethernet card) as the Mac itself, under the Mac's
// hardware address, so that on the network the dialled-in Mac looks like
// the Mac on Ethernet: same address, its own IPv4 address from the router.
//
//  - DHCP (RFC 2131, options RFC 2132), as the Mac's client: DISCOVER,
//    OFFER, REQUEST, ACK; asks for subnet mask (1), router (3), DNS (6)
//    and the lease time (51); renews by unicast REQUEST at half the lease.
//    BOOTP layout: op, htype 1, hlen 6, hops, xid, secs, flags (0x8000
//    BROADCAST), ciaddr, yiaddr, siaddr, giaddr, chaddr (16), sname (64),
//    file (128), magic cookie 99.130.83.99, options. Client port 68, server
//    port 67. Retries every 4 s.
//  - ARP (RFC 826) over Ethernet: answers requests for the Mac's address
//    with the Mac's hardware address; resolves next hops (the router, or
//    a host on the same subnet) and holds a few packets meanwhile.
//  - IPv4 to the Mac's address goes to the Mac; nothing else does.
//
// Freestanding; one thread.

#ifndef G3_PLATFORM_COMMON_DIALUP_LINK_H
#define G3_PLATFORM_COMMON_DIALUP_LINK_H

#include "core/common/types.h"
#include "platform/common/ppp_server.h"

namespace g3 {

class DialupLink {
public:
    using FrameFn = void (*)(void* context, const u8* frame, u32 len);    // to the network
    using PacketFn = void (*)(void* context, const u8* packet, u32 len);  // IPv4 to the Mac
    using ReadyFn = void (*)(void* context, const PppServer::Addresses& a);

    explicit DialupLink(const u8 mac_address[6]);
    void set_network(FrameFn fn, void* ctx) { net_ = fn; net_ctx_ = ctx; }
    void set_mac_side(PacketFn fn, void* ctx) { to_mac_ = fn; to_mac_ctx_ = ctx; }
    void set_ready(ReadyFn fn, void* ctx) { ready_ = fn; ready_ctx_ = ctx; }

    void start(u64 now_us);   // the call is up: get an address
    void stop();              // the call is down
    void from_mac(const u8* packet, u32 len, u64 now_us);
    void from_network(const u8* frame, u32 len, u64 now_us);
    void tick(u64 now_us);

    bool ready() const { return state_ == State::Bound; }
    const PppServer::Addresses& addresses() const { return addr_; }
    u32 netmask() const { return mask_; }

    static constexpr u64 kDhcpRetryUs = 4000000, kArpRetryUs = 1000000;
    static constexpr u32 kArpTries = 3, kArpCache = 16, kHeld = 8;

private:
    enum class State : u8 { Off, Selecting, Requesting, Bound };
    void dhcp_send(u8 type, u64 now_us);
    void dhcp_reply(const u8* bootp, u32 len, u64 now_us);
    void send_frame(const u8 dst[6], u16 type, const u8* payload, u32 len);
    void send_arp(u16 op, const u8 target_mac[6], u32 target_ip);
    void arp_frame(const u8* arp, u64 now_us);
    void route(const u8* packet, u32 len, u64 now_us);
    bool lookup(u32 ip, u8 out[6]) const;
    void learn(u32 ip, const u8 mac[6], u64 now_us);

    u8 mac_[6];
    State state_ = State::Off;
    u32 xid_ = 0;
    u64 dhcp_at_ = 0, renew_at_ = 0;
    u32 offered_ = 0, server_id_ = 0, lease_s_ = 0;
    PppServer::Addresses addr_;
    u32 router_ = 0, mask_ = 0;

    struct ArpEntry { u32 ip = 0; u8 mac[6] = {}; bool valid = false; };
    ArpEntry cache_[kArpCache];
    u32 cache_next_ = 0;
    struct Held { u32 hop = 0; u32 len = 0; u8 data[1500]; bool used = false; };
    Held held_[kHeld];
    u32 arp_for_ = 0, arp_tries_ = 0;
    u64 arp_at_ = 0;

    FrameFn net_ = nullptr;
    void* net_ctx_ = nullptr;
    PacketFn to_mac_ = nullptr;
    void* to_mac_ctx_ = nullptr;
    ReadyFn ready_ = nullptr;
    void* ready_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_DIALUP_LINK_H
