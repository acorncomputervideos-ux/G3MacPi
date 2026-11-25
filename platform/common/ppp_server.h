// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/ppp_server.h — the dial-up provider's PPP end, what the
// Mac's Remote Access talks to once the modem has connected
// (docs/devices/modem.md). It runs the link (LCP, RFC 1661), accepts any
// name and password (PAP, RFC 1334), hands out an IPv4 address and DNS
// servers (IPCP, RFC 1332 and RFC 1877), then passes IP packets.
//
// Packets of LCP, PAP and IPCP: code (1), identifier (1), length (2,
// header included), data. Configure options: type (1), length (1,
// including these two bytes), value.
//  - LCP codes: 1 Configure-Request, 2 -Ack, 3 -Nak, 4 -Reject,
//    5 Terminate-Request, 6 Terminate-Ack, 7 Code-Reject, 8 Protocol-Reject,
//    9 Echo-Request, 10 Echo-Reply, 11 Discard-Request. Options: 1 MRU,
//    2 ACCM, 3 Authentication-Protocol, 4 Quality-Protocol, 5 Magic-Number,
//    7 Protocol-Field-Compression, 8 Address-and-Control-Field-Compression.
//  - PAP (0xC023): 1 Authenticate-Request (peer-id and password, each
//    length-prefixed), 2 Authenticate-Ack, 3 Authenticate-Nak.
//  - IPCP (0x8021), same codes 1-7. Options: 2 IP-Compression-Protocol,
//    3 IP-Address, 129 Primary DNS, 130 Primary NBNS, 131 Secondary DNS,
//    132 Secondary NBNS.
//  - Protocols: 0x0021 IP, 0x8021 IPCP, 0xC021 LCP, 0xC023 PAP.
//  - Restart timer 3 s, Max-Configure 10 (RFC 1661's defaults).
// Until LCP is open, frames go out with every control character escaped
// and nothing compressed (RFC 1662); after, as the Mac asked.
//
// Freestanding; one thread.

#ifndef G3_PLATFORM_COMMON_PPP_SERVER_H
#define G3_PLATFORM_COMMON_PPP_SERVER_H

#include "core/common/types.h"
#include "platform/common/ppp_framer.h"

namespace g3 {

class PppServer {
public:
    struct Addresses {
        u32 client = 0;  // the Mac's IPv4 address (host order)
        u32 server = 0;  // this end's
        u32 dns1 = 0, dns2 = 0;
    };
    using BytesFn = void (*)(void* context, const u8* data, u32 len);  // to the Mac's serial line
    using PacketFn = void (*)(void* context, const u8* packet, u32 len);  // IP from the Mac
    using HangUpFn = void (*)(void* context);                           // the Mac ended the link

    PppServer();
    void set_output(BytesFn fn, void* ctx) { out_ = fn; out_ctx_ = ctx; }
    void set_ip_receiver(PacketFn fn, void* ctx) { ip_ = fn; ip_ctx_ = ctx; }
    void set_hang_up(HangUpFn fn, void* ctx) { hang_up_ = fn; hang_up_ctx_ = ctx; }

    void start(u64 now_us);           // the call is up: LCP begins
    void stop();                      // the call is down
    void set_addresses(const Addresses& a, u64 now_us);  // IPCP can go on when known
    void receive(u8 byte, u64 now_us);  // from the Mac
    void tick(u64 now_us);
    bool send_ip(const u8* packet, u32 len);  // to the Mac; false before IPCP is open

    bool running() const { return running_; }
    bool lcp_open() const { return lcp_.mine_acked && lcp_.theirs_acked; }
    bool authenticated() const { return authed_; }
    bool ip_up() const { return ipcp_.mine_acked && ipcp_.theirs_acked && lcp_open() && authed_; }
    const Addresses& addresses() const { return addr_; }

    static constexpr u16 kLcp = 0xC021, kPap = 0xC023, kIpcp = 0x8021, kIp = 0x0021;
    static constexpr u64 kRestartUs = 3000000;
    static constexpr u32 kMaxConfigure = 10;

private:
    struct Cp {             // a control protocol's negotiation state
        bool mine_acked = false, theirs_acked = false;
        u8 id = 0;          // identifier of our last Configure-Request
        u32 tries = 0;
        u64 resend_at = 0;
    };
    void frame(u16 protocol, const u8* info, u32 len);
    void on_frame(u16 protocol, const u8* info, u32 len);
    void send_cp(u16 protocol, u8 code, u8 id, const u8* data, u32 len);
    void lcp_packet(u8 code, u8 id, const u8* data, u32 len, const u8* whole, u32 whole_len);
    void lcp_request();
    void lcp_their_request(u8 id, const u8* opts, u32 len);
    void pap_packet(u8 code, u8 id, const u8* data, u32 len);
    void ipcp_packet(u8 code, u8 id, const u8* data, u32 len);
    void ipcp_request();
    void ipcp_their_request(u8 id, const u8* opts, u32 len);
    void maybe_start_ipcp();
    void finish();

    PppFramer framer_;
    u64 now_ = 0;
    bool running_ = false;
    // LCP: what we ask for, and what the Mac asked for (how we send).
    Cp lcp_;
    bool want_pap_ = true, want_pfc_ = true, want_acfc_ = true, want_accm_ = true;
    u32 magic_ = 0;
    PppFramer::Options tx_;          // used once LCP is open
    PppFramer::Options agreed_;      // what the Mac's acknowledged request allows
    bool authed_ = false;
    // IPCP.
    Cp ipcp_;
    bool have_addresses_ = false, ipcp_started_ = false;
    Addresses addr_;
    bool terminating_ = false;
    u8 next_id_ = 1;

    BytesFn out_ = nullptr;
    void* out_ctx_ = nullptr;
    PacketFn ip_ = nullptr;
    void* ip_ctx_ = nullptr;
    HangUpFn hang_up_ = nullptr;
    void* hang_up_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_PPP_SERVER_H
