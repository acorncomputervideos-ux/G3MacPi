// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/host/sim_lan.cpp — see sim_lan.h. Formats: Ethernet II, ARP (RFC 826),
// IPv4 (RFC 791), ICMP echo (RFC 792), UDP (RFC 768), DHCP (RFC 2131/2132),
// DNS (RFC 1035), TCP (RFC 793, just enough for one HTTP/1.0 exchange).

#include "app/host/sim_lan.h"

#include <cstdio>
#include <cstring>

namespace g3 {

namespace {
const u8 kRouterMac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
u16 be16(const u8* p) { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return static_cast<u32>(be16(p)) << 16 | be16(p + 2); }
void put16(std::vector<u8>& v, u32 x) { v.push_back(static_cast<u8>(x >> 8)); v.push_back(static_cast<u8>(x)); }
void put32(std::vector<u8>& v, u32 x) { put16(v, x >> 16); put16(v, x); }
void set16(std::vector<u8>& v, size_t o, u32 x) { v[o] = static_cast<u8>(x >> 8); v[o + 1] = static_cast<u8>(x); }
u32 sum16(const u8* p, size_t n, u32 sum = 0)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += be16(p + i);
    if (n & 1)
        sum += static_cast<u32>(p[n - 1]) << 8;
    return sum;
}
u16 fold(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}
std::string ip_text(u32 ip)
{
    char b[20];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

const char kPage[] =
    "<html><head><title>G3MacPi dial-up</title></head>"
    "<body bgcolor=\"#336699\" text=\"#FFFFFF\"><center><br><br>"
    "<h1>Connected!</h1>"
    "<h2>This page came down the G3MacPi virtual 56K modem.</h2>"
    "<p>Remote Access dialled, the modem answered, PPP gave the Mac an address,"
    " and this page travelled over TCP/IP through the modem.</p>"
    "<p><i>(Served by the test tool's simulated network.)</i></p>"
    "</center></body></html>";
} // namespace

void SimLan::reply_frame(const u8 dst[6], u16 type, const std::vector<u8>& payload)
{
    std::vector<u8> f(dst, dst + 6);
    f.insert(f.end(), kRouterMac, kRouterMac + 6);
    put16(f, type);
    f.insert(f.end(), payload.begin(), payload.end());
    while (f.size() < 60)
        f.push_back(0);
    out_.push_back(std::move(f));
}

void SimLan::ip_out(const u8 dst_mac[6], u32 src, u32 dst, u8 proto, const std::vector<u8>& payload)
{
    std::vector<u8> ip;
    ip.push_back(0x45);
    ip.push_back(0);
    put16(ip, static_cast<u32>(20 + payload.size()));
    put16(ip, ip_id_++);
    put16(ip, 0);
    ip.push_back(64);
    ip.push_back(proto);
    put16(ip, 0);
    put32(ip, src);
    put32(ip, dst);
    set16(ip, 10, fold(sum16(ip.data(), 20)));
    ip.insert(ip.end(), payload.begin(), payload.end());
    reply_frame(dst_mac, 0x0800, ip);
}

void SimLan::from_mac(const u8* f, u32 len)
{
    if (len < 14)
        return;
    const u16 type = be16(f + 12);
    if (type == 0x0806 && len >= 42) {  // ARP: we are the router (and any other address asked for)
        const u8* a = f + 14;
        const u32 tpa = be32(a + 24);
        if (be16(a + 6) == 1 && tpa != kClientIp && tpa != be32(a + 14)) {
            std::vector<u8> r;
            put16(r, 1);
            put16(r, 0x0800);
            r.push_back(6);
            r.push_back(4);
            put16(r, 2);
            r.insert(r.end(), kRouterMac, kRouterMac + 6);
            put32(r, tpa);
            r.insert(r.end(), a + 8, a + 14);
            r.insert(r.end(), a + 14, a + 18);
            reply_frame(a + 8, 0x0806, r);
        }
        return;
    }
    if (type != 0x0800 || len < 34)
        return;
    const u8* ip = f + 14;
    const u32 ihl = (ip[0] & 15u) * 4, total = be16(ip + 2);
    if (total > len - 14 || total < ihl)
        return;
    const u8* body = ip + ihl;
    const u32 blen = total - ihl;
    const u8 proto = ip[9];
    if (proto == 17 && blen >= 8) {
        if (be16(body + 2) == 67)
            dhcp(f, ip, body, blen);
        else if (be16(body + 2) == 53)
            dns(f, ip, body, blen);
    } else if (proto == 1 && blen >= 8 && body[0] == 8) {  // ICMP echo request: echo reply from that address
        std::vector<u8> icmp(body, body + blen);
        icmp[0] = 0;
        set16(icmp, 2, 0);
        set16(icmp, 2, fold(sum16(icmp.data(), icmp.size())));
        ip_out(f + 6, be32(ip + 16), be32(ip + 12), 1, icmp);
        log.push_back("ping " + ip_text(be32(ip + 16)) + " answered");
    } else if (proto == 6 && blen >= 20) {
        tcp(f, ip, body, blen);
    }
}

void SimLan::dhcp(const u8* f, const u8*, const u8* udp, u32 udp_len)
{
    const u8* b = udp + 8;
    const u32 n = udp_len - 8;
    if (n < 240 || b[0] != 1)
        return;
    u8 type = 0;
    for (u32 o = 240; o + 1 < n && b[o] != 255;) {
        if (b[o] == 0) { ++o; continue; }
        if (b[o] == 53)
            type = b[o + 2];
        o += 2 + b[o + 1];
    }
    if (type != 1 && type != 3)
        return;
    std::vector<u8> r(b, b + 236);
    r[0] = 2;
    std::memset(r.data() + 12, 0, 16);
    r[16] = 192; r[17] = 168; r[18] = 1; r[19] = 50;   // yiaddr
    r[20] = 192; r[21] = 168; r[22] = 1; r[23] = 1;    // siaddr
    const u8 opts[] = {99, 130, 83, 99, 53, 1, static_cast<u8>(type == 1 ? 2 : 5), 54, 4, 192, 168, 1, 1,
                       1, 4, 255, 255, 255, 0, 3, 4, 192, 168, 1, 1, 6, 4, 192, 168, 1, 1, 51, 4, 0, 1, 0x51, 0x80, 255};
    r.insert(r.end(), opts, opts + sizeof opts);
    std::vector<u8> u;
    put16(u, 67);
    put16(u, 68);
    put16(u, static_cast<u32>(8 + r.size()));
    put16(u, 0);
    u.insert(u.end(), r.begin(), r.end());
    const u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ip_out(bcast, kRouterIp, 0xFFFFFFFF, 17, u);
    log.push_back(type == 1 ? "DHCP offer 192.168.1.50" : "DHCP ack 192.168.1.50");
    (void)f;
}

void SimLan::dns(const u8* f, const u8* ip, const u8* udp, u32 udp_len)
{
    const u8* q = udp + 8;
    const u32 n = udp_len - 8;
    if (n < 12 || be16(q + 4) != 1)
        return;
    u32 o = 12;
    std::string name;
    while (o < n && q[o]) {
        if (!name.empty())
            name += '.';
        name.append(reinterpret_cast<const char*>(q + o + 1), q[o]);
        o += 1 + q[o];
    }
    o += 5;  // zero, type, class
    if (o > n)
        return;
    std::vector<u8> r(q, q + o);
    r[2] = 0x81;  // response, recursion desired
    r[3] = 0x80;  // recursion available, no error
    set16(r, 6, 1);
    const u8 ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0x0E, 0x10, 0, 4, 192, 168, 1, 1};
    r.insert(r.end(), ans, ans + sizeof ans);
    std::vector<u8> u;
    put16(u, 53);
    put16(u, be16(udp));
    put16(u, static_cast<u32>(8 + r.size()));
    put16(u, 0);
    u.insert(u.end(), r.begin(), r.end());
    ip_out(f + 6, be32(ip + 16), be32(ip + 12), 17, u);
    log.push_back("DNS " + name + " -> 192.168.1.1");
}

void SimLan::tcp(const u8* f, const u8* ip, const u8* seg, u32 seg_len)
{
    const u32 src = be32(ip + 12), dst = be32(ip + 16);
    const u16 sport = be16(seg), dport = be16(seg + 2);
    const u32 seq = be32(seg + 4), ack = be32(seg + 8);
    const u32 off = (seg[12] >> 4) * 4u;
    const u8 flags = seg[13];
    if (dport != 80 || off > seg_len)
        return;
    auto send = [&](Conn& c, u8 fl, const std::string& data) {
        std::vector<u8> t;
        put16(t, 80);
        put16(t, c.port);
        put32(t, c.snd_nxt);
        put32(t, c.rcv_nxt);
        t.push_back(0x50);
        t.push_back(fl);
        put16(t, 8192);
        put16(t, 0);
        put16(t, 0);
        t.insert(t.end(), data.begin(), data.end());
        std::vector<u8> pseudo;
        put32(pseudo, dst);
        put32(pseudo, src);
        put16(pseudo, 6);
        put16(pseudo, static_cast<u32>(t.size()));
        set16(t, 16, fold(sum16(t.data(), t.size(), sum16(pseudo.data(), pseudo.size()))));
        ip_out(f + 6, dst, src, 6, t);
        c.snd_nxt += static_cast<u32>(data.size()) + ((fl & 0x03) ? 1 : 0);  // SYN and FIN count one
    };
    Conn* c = nullptr;
    for (Conn& k : conns_)
        if (k.ip == src && k.port == sport)
            c = &k;
    if (flags & 0x02) {  // SYN
        if (!c) {
            conns_.push_back(Conn{});
            c = &conns_.back();
        }
        *c = Conn{};
        c->ip = src;
        c->port = sport;
        c->snd_nxt = 0x10000000u;
        c->rcv_nxt = seq + 1;
        send(*c, 0x12, "");  // SYN+ACK
        log.push_back("TCP connection from " + ip_text(src) + " to port 80");
        return;
    }
    if (!c)
        return;
    (void)ack;
    const u32 data_len = seg_len - off;
    if (data_len && seq == c->rcv_nxt) {
        c->request.append(reinterpret_cast<const char*>(seg + off), data_len);
        c->rcv_nxt += data_len;
        send(*c, 0x10, "");  // ACK
    }
    if (!c->sent_page && c->request.find("\r\n\r\n") != std::string::npos) {
        c->sent_page = true;
        const std::string body = kPage;
        const std::string resp = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        for (size_t i = 0; i < resp.size(); i += 512)
            send(*c, 0x18, resp.substr(i, 512));  // PSH+ACK
        send(*c, 0x11, "");  // FIN+ACK
        log.push_back("HTTP: sent the page (" + std::to_string(resp.size()) + " bytes)");
    }
    if (flags & 0x01) {  // FIN from the Mac
        c->rcv_nxt = seq + data_len + 1;
        send(*c, 0x10, "");
    }
}

u32 SimLan::to_mac(u8* buf)
{
    if (out_.empty())
        return 0;
    const std::vector<u8> f = std::move(out_.front());
    out_.pop_front();
    std::memcpy(buf, f.data(), f.size());
    return static_cast<u32>(f.size());
}

} // namespace g3
