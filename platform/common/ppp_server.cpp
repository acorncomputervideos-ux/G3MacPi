// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/ppp_server.cpp — see ppp_server.h.

#include "platform/common/ppp_server.h"

namespace g3 {

namespace {
constexpr u8 kConfReq = 1, kConfAck = 2, kConfNak = 3, kConfRej = 4, kTermReq = 5, kTermAck = 6, kCodeRej = 7,
             kProtoRej = 8, kEchoReq = 9, kEchoRep = 10;

u32 be32(const u8* p) { return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | p[2] << 8 | p[3]; }
void put32(u8* p, u32 v)
{
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

// Up to 256 bytes of options being built (reject / nak / ack lists).
struct OptList {
    u8 b[256];
    u32 n = 0;
    void add(const u8* opt, u32 len)
    {
        for (u32 i = 0; i < len && n < sizeof b; ++i)
            b[n++] = opt[i];
    }
    void add_addr(u8 type, u32 v)
    {
        u8 o[6] = {type, 6};
        put32(o + 2, v);
        add(o, 6);
    }
};
} // namespace

PppServer::PppServer()
{
    framer_.set_receiver([](void* c, u16 p, const u8* d, u32 n) { static_cast<PppServer*>(c)->on_frame(p, d, n); },
                         this);
}

void PppServer::start(u64 now_us)
{
    now_ = now_us;
    lcp_ = Cp{};
    ipcp_ = Cp{};
    want_pap_ = want_pfc_ = want_acfc_ = want_accm_ = true;
    magic_ = 0x47334D50u ^ static_cast<u32>(now_us);  // "G3MP", varied by the time
    if (!magic_)
        magic_ = 1;
    tx_ = PppFramer::Options{};
    agreed_ = PppFramer::Options{};
    authed_ = false;
    ipcp_started_ = false;
    terminating_ = false;
    running_ = true;
    lcp_request();
}

void PppServer::stop() { running_ = false; }

void PppServer::set_addresses(const Addresses& a, u64 now_us)
{
    now_ = now_us;
    addr_ = a;
    have_addresses_ = true;
    maybe_start_ipcp();
}

void PppServer::receive(u8 byte, u64 now_us)
{
    now_ = now_us;
    if (running_)
        framer_.receive(byte);
}

void PppServer::finish()
{
    running_ = false;
    if (hang_up_)
        hang_up_(hang_up_ctx_);
}

// --- sending ----------------------------------------------------------------------

void PppServer::frame(u16 protocol, const u8* info, u32 len)
{
    static u8 buf[2 * (PppFramer::kMaxInfo + 6) + 2];
    if (len > PppFramer::kMaxInfo)
        return;
    const PppFramer::Options opt = lcp_open() ? tx_ : PppFramer::Options{};
    const u32 n = PppFramer::encode(protocol, info, len, opt, buf);
    if (out_)
        out_(out_ctx_, buf, n);
}

void PppServer::send_cp(u16 protocol, u8 code, u8 id, const u8* data, u32 len)
{
    u8 p[4 + 256 + 4];
    if (len > 256)
        len = 256;
    p[0] = code;
    p[1] = id;
    p[2] = static_cast<u8>((len + 4) >> 8);
    p[3] = static_cast<u8>(len + 4);
    for (u32 i = 0; i < len; ++i)
        p[4 + i] = data[i];
    frame(protocol, p, len + 4);
}

bool PppServer::send_ip(const u8* packet, u32 len)
{
    if (!running_ || !ip_up())
        return false;
    frame(kIp, packet, len);
    return true;
}

// --- receiving --------------------------------------------------------------------

void PppServer::on_frame(u16 protocol, const u8* info, u32 len)
{
    if (protocol == kLcp || protocol == kPap || protocol == kIpcp) {
        if (len < 4)
            return;
        const u32 plen = static_cast<u32>(info[2]) << 8 | info[3];
        if (plen < 4 || plen > len)
            return;  // malformed: discarded
        if (protocol == kLcp) {
            lcp_packet(info[0], info[1], info + 4, plen - 4, info, plen);
            return;
        }
        if (!lcp_open())
            return;  // only LCP before the link is open (RFC 1661 3.4)
        if (protocol == kPap)
            pap_packet(info[0], info[1], info + 4, plen - 4);
        else if (authed_)
            ipcp_packet(info[0], info[1], info + 4, plen - 4);
        return;
    }
    if (!lcp_open())
        return;
    if (protocol == kIp) {
        if (ip_up() && ip_)
            ip_(ip_ctx_, info, len);
        return;
    }
    // Anything else (AppleTalk's ATCP, CCP, IPv6CP ...): Protocol-Reject.
    u8 d[2 + 128];
    d[0] = static_cast<u8>(protocol >> 8);
    d[1] = static_cast<u8>(protocol);
    const u32 n = len < 128 ? len : 128;
    for (u32 i = 0; i < n; ++i)
        d[2 + i] = info[i];
    send_cp(kLcp, kProtoRej, next_id_++, d, 2 + n);
}

// --- LCP --------------------------------------------------------------------------

void PppServer::lcp_request()
{
    OptList o;
    if (want_accm_) {
        const u8 accm[6] = {2, 6, 0, 0, 0, 0};  // we take every control character unescaped
        o.add(accm, 6);
    }
    if (want_pap_) {
        const u8 auth[4] = {3, 4, 0xC0, 0x23};
        o.add(auth, 4);
    }
    if (magic_) {
        u8 m[6] = {5, 6};
        put32(m + 2, magic_);
        o.add(m, 6);
    }
    if (want_pfc_) {
        const u8 pfc[2] = {7, 2};
        o.add(pfc, 2);
    }
    if (want_acfc_) {
        const u8 acfc[2] = {8, 2};
        o.add(acfc, 2);
    }
    lcp_.id = next_id_++;
    ++lcp_.tries;
    lcp_.resend_at = now_ + kRestartUs;
    send_cp(kLcp, kConfReq, lcp_.id, o.b, o.n);
}

void PppServer::lcp_their_request(u8 id, const u8* opts, u32 len)
{
    OptList rej, nak;
    PppFramer::Options agree;
    agree.accm = 0xFFFFFFFFu;
    for (u32 i = 0; i < len;) {
        if (len - i < 2 || opts[i + 1] < 2 || i + opts[i + 1] > len)
            return;  // malformed: discarded
        const u8 t = opts[i], l = opts[i + 1];
        const u8* v = opts + i + 2;
        bool ok = true;
        switch (t) {
        case 1: ok = l == 4; break;                                     // MRU: any
        case 2: ok = l == 6; if (ok) agree.accm = be32(v); break;       // how we must send
        case 5: ok = l == 6; break;                                     // magic number
        case 7: ok = l == 2; agree.pfc = ok; break;
        case 8: ok = l == 2; agree.acfc = ok; break;
        default: ok = false; break;                                     // auth, quality, others
        }
        if (!ok)
            rej.add(opts + i, l);
        i += l;
    }
    if (rej.n) {
        send_cp(kLcp, kConfRej, id, rej.b, rej.n);
        return;
    }
    if (nak.n) {
        send_cp(kLcp, kConfNak, id, nak.b, nak.n);
        return;
    }
    send_cp(kLcp, kConfAck, id, opts, len);
    const bool was_open = lcp_open();
    lcp_.theirs_acked = true;
    agreed_ = agree;
    if (!was_open && lcp_open()) {
        tx_ = agreed_;
        authed_ = !want_pap_;
        maybe_start_ipcp();
    }
}

void PppServer::lcp_packet(u8 code, u8 id, const u8* data, u32 len, const u8* whole, u32 whole_len)
{
    switch (code) {
    case kConfReq:
        lcp_their_request(id, data, len);
        return;
    case kConfAck:
        if (id == lcp_.id && !lcp_.mine_acked) {
            lcp_.mine_acked = true;
            if (lcp_open()) {
                tx_ = agreed_;
                authed_ = !want_pap_;
                maybe_start_ipcp();
            }
        }
        return;
    case kConfNak:
    case kConfRej:
        if (id != lcp_.id || lcp_.mine_acked)
            return;
        for (u32 i = 0; i + 2 <= len && data[i + 1] >= 2 && i + data[i + 1] <= len; i += data[i + 1]) {
            switch (data[i]) {
            case 2: want_accm_ = false; break;
            case 3: want_pap_ = false; break;   // another protocol, or none: no authentication
            case 5:
                if (code == kConfRej)
                    magic_ = 0;
                else
                    magic_ = magic_ * 1103515245u + 12345u;  // looped back or unwanted: another one
                break;
            case 7: want_pfc_ = false; break;
            case 8: want_acfc_ = false; break;
            default: break;
            }
        }
        lcp_request();  // (counts towards Max-Configure, so a Mac that never agrees ends the call)
        return;
    case kTermReq:
        send_cp(kLcp, kTermAck, id, nullptr, 0);
        finish();
        return;
    case kTermAck:
        if (terminating_)
            finish();
        return;
    case kCodeRej:
    case kProtoRej:
    case kEchoRep:
    case 11:  // Discard-Request
        return;
    case kEchoReq: {
        if (!lcp_open())
            return;
        u8 d[4 + 64];
        put32(d, magic_);
        u32 n = len > 4 ? len - 4 : 0;
        if (n > 64)
            n = 64;
        for (u32 i = 0; i < n; ++i)
            d[4 + i] = data[4 + i];
        send_cp(kLcp, kEchoRep, id, d, 4 + n);
        return;
    }
    default: {
        const u32 n = whole_len < 200 ? whole_len : 200;
        send_cp(kLcp, kCodeRej, next_id_++, whole, n);
        return;
    }
    }
}

// --- PAP --------------------------------------------------------------------------

void PppServer::pap_packet(u8 code, u8 id, const u8*, u32)
{
    if (code != 1 || !want_pap_)
        return;
    static const char kWelcome[] = "Welcome to G3MacPi";
    u8 d[1 + sizeof kWelcome];
    d[0] = static_cast<u8>(sizeof kWelcome - 1);
    for (u32 i = 0; i + 1 < sizeof kWelcome; ++i)
        d[1 + i] = static_cast<u8>(kWelcome[i]);
    // Authenticate-Ack: any name and password will do.
    u8 p[4 + sizeof d];
    p[0] = 2;
    p[1] = id;
    p[2] = 0;
    p[3] = static_cast<u8>(4 + sizeof kWelcome);
    for (u32 i = 0; i < sizeof kWelcome; ++i)
        p[4 + i] = d[i];
    frame(kPap, p, 4 + sizeof kWelcome);
    if (!authed_) {
        authed_ = true;
        maybe_start_ipcp();
    }
}

// --- IPCP -------------------------------------------------------------------------

void PppServer::maybe_start_ipcp()
{
    if (!running_ || !lcp_open() || !authed_ || !have_addresses_ || ipcp_started_)
        return;
    ipcp_started_ = true;
    ipcp_ = Cp{};
    ipcp_request();
}

void PppServer::ipcp_request()
{
    OptList o;
    o.add_addr(3, addr_.server);
    ipcp_.id = next_id_++;
    ++ipcp_.tries;
    ipcp_.resend_at = now_ + kRestartUs;
    send_cp(kIpcp, kConfReq, ipcp_.id, o.b, o.n);
}

void PppServer::ipcp_their_request(u8 id, const u8* opts, u32 len)
{
    OptList rej, nak;
    bool asked_address = false;
    for (u32 i = 0; i < len;) {
        if (len - i < 2 || opts[i + 1] < 2 || i + opts[i + 1] > len)
            return;
        const u8 t = opts[i], l = opts[i + 1];
        const u8* v = opts + i + 2;
        u32 want = 0;
        bool known = l == 6;
        switch (t) {
        case 3: want = addr_.client; asked_address = true; break;
        case 129: want = addr_.dns1; known = known && want != 0; break;
        case 131: want = addr_.dns2; known = known && want != 0; break;
        default: known = false; break;  // VJ compression, NBNS, others
        }
        if (!known)
            rej.add(opts + i, l);
        else if (be32(v) != want)
            nak.add_addr(t, want);
        i += l;
    }
    if (rej.n) {
        send_cp(kIpcp, kConfRej, id, rej.b, rej.n);
        return;
    }
    if (!asked_address)
        nak.add_addr(3, addr_.client);  // it must take this one (RFC 1332 3.3)
    if (nak.n) {
        send_cp(kIpcp, kConfNak, id, nak.b, nak.n);
        return;
    }
    send_cp(kIpcp, kConfAck, id, opts, len);
    ipcp_.theirs_acked = true;
}

void PppServer::ipcp_packet(u8 code, u8 id, const u8* data, u32 len)
{
    if (!ipcp_started_)
        return;  // no addresses yet: the Mac asks again
    switch (code) {
    case kConfReq:
        ipcp_their_request(id, data, len);
        return;
    case kConfAck:
        if (id == ipcp_.id)
            ipcp_.mine_acked = true;
        return;
    case kConfNak:
    case kConfRej:
        if (id == ipcp_.id && !ipcp_.mine_acked)
            ipcp_request();  // our address is ours to choose: asked again (counted)
        return;
    case kTermReq:
        send_cp(kIpcp, kTermAck, id, nullptr, 0);
        ipcp_.mine_acked = ipcp_.theirs_acked = false;
        return;
    default:
        return;
    }
}

// --- timers -----------------------------------------------------------------------

void PppServer::tick(u64 now_us)
{
    now_ = now_us;
    if (!running_)
        return;
    if (!lcp_.mine_acked && now_us >= lcp_.resend_at) {
        if (lcp_.tries >= kMaxConfigure) {
            finish();  // the Mac is not answering: hang up
            return;
        }
        lcp_request();
    }
    if (ipcp_started_ && !ipcp_.mine_acked && now_us >= ipcp_.resend_at) {
        if (ipcp_.tries >= kMaxConfigure) {
            finish();
            return;
        }
        ipcp_request();
    }
}

} // namespace g3
