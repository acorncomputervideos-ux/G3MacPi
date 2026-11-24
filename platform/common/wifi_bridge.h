// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/wifi_bridge.h — the Mac on the Pi's WiFi.
//
// On Ethernet the Mac has its own hardware address (net_switch.h). A WiFi
// client cannot do that: an access point accepts frames only from the
// address of the station that joined it (IEEE 802.11 data frames carry the
// transmitter's address, and a station has one). So the Mac's frames go out
// under the Pi's address, and frames that come back to the Pi's address are
// sorted between the Pi and the Mac by what is inside them. This is the
// "MAC address translation" used for wireless bridging by e.g. VirtualBox.
//
// Out from the Mac (to the air):
//   - the source address becomes the Pi's;
//   - ARP (RFC 826): the sender hardware address becomes the Pi's; the
//     Mac's IPv4 address is learnt from its ARP and IPv4 frames;
//   - DHCP requests (RFC 2131): the BROADCAST flag is set, so the server's
//     answer is broadcast (it could not be unicast to the Mac's own
//     address), and the UDP checksum is cleared (allowed over IPv4, RFC 768);
//   - AARP (Inside AppleTalk, 2nd ed., ch. 2): the source hardware address
//     becomes the Pi's.
// In from the air:
//   - frames the Pi sent itself (the access point repeats broadcasts) are
//     dropped;
//   - broadcasts and multicasts go to both, unchanged;
//   - to the Pi's address: ARP for the Mac's IPv4 address, IPv4 to it, DHCP
//     replies for the Mac's hardware address, and AppleTalk (DDP, AARP;
//     the Pi's own stack speaks neither) go to the Mac, the destination
//     (and the ARP/AARP target) changed to the Mac's address; all else to
//     the Pi.
// Between the Mac and the Pi, frames are delivered directly (the access
// point does not send a station's frames back to it): unchanged, as on a
// wire.
//
// Freestanding; one thread (the frame switch).

#ifndef G3_PLATFORM_COMMON_WIFI_BRIDGE_H
#define G3_PLATFORM_COMMON_WIFI_BRIDGE_H

#include "core/common/types.h"

namespace g3 {

class WifiBridge {
public:
    enum : u32 { kToAir = 1, kToMac = 2, kToHost = 4 };

    WifiBridge(const u8 host_address[6], const u8 mac_address[6]);

    // A frame from the Mac: where it goes (kToHost and/or kToAir). Learns
    // the Mac's IPv4 address. Give the host its copy before to_air().
    u32 route_from_mac(const u8* frame, u32 len);
    // Changes a frame from the Mac in place for the air (same length).
    void to_air(u8* frame, u32 len) const;

    // A frame from the host's stack: kToAir and/or kToMac (unchanged).
    u32 route_from_host(const u8* frame, u32 len) const;

    // A frame from the air: kToMac and/or kToHost, or 0 (dropped). A frame
    // for the Mac alone is changed in place for it.
    u32 from_air(u8* frame, u32 len);

    // The Mac's IPv4 address as learnt (0: not yet), most significant byte first.
    u32 mac_ip() const { return mac_ip_; }

private:
    u8 host_[6];
    u8 mac_[6];
    u32 mac_ip_ = 0;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_WIFI_BRIDGE_H
