// SPDX-License-Identifier: GPL-3.0-or-later
//
// app/host/sim_lan.h — a small simulated network for g3_host (G3_SIMLAN=1),
// standing in for a home network and the internet when the test runner has
// no real one: a "router" at 192.168.1.1 that answers DHCP (192.168.1.50
// for the first client), ARP for itself, ICMP echo for any address, DNS for
// any name (with its own address), and HTTP on port 80 with one page.
// Enough to show the Mac getting on line (Ethernet or the dial-up modem)
// and loading a web page. Not part of the emulator.

#ifndef G3_APP_HOST_SIM_LAN_H
#define G3_APP_HOST_SIM_LAN_H

#include "core/common/types.h"

#include <deque>
#include <string>
#include <vector>

namespace g3 {

class SimLan {
public:
    void from_mac(const u8* frame, u32 len);   // a frame sent by the Mac
    u32 to_mac(u8* buf);                       // the next frame for the Mac (0: none)
    std::vector<std::string> log;              // what happened, for the test output

    static constexpr u32 kRouterIp = 0xC0A80101, kClientIp = 0xC0A80132;

private:
    void reply_frame(const u8 dst[6], u16 type, const std::vector<u8>& payload);
    void ip_out(const u8 dst_mac[6], u32 src, u32 dst, u8 proto, const std::vector<u8>& payload);
    void dhcp(const u8* f, const u8* ip, const u8* udp, u32 udp_len);
    void dns(const u8* f, const u8* ip, const u8* udp, u32 udp_len);
    void tcp(const u8* f, const u8* ip, const u8* seg, u32 seg_len);

    struct Conn {
        u32 ip = 0;
        u16 port = 0, our_port = 0;
        u32 snd_nxt = 0, rcv_nxt = 0;
        bool sent_page = false;
        std::string request;
    };
    std::vector<Conn> conns_;
    std::deque<std::vector<u8>> out_;
    u16 ip_id_ = 1;
};

} // namespace g3

#endif // G3_APP_HOST_SIM_LAN_H
