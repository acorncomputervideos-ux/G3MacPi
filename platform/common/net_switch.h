// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/net_switch.h — one Ethernet port shared by two stations:
// the Mac (the emulated BMAC+) and the host's own network stack (the Pi's
// web interface). The I/O core reads every frame from the adapter and
// routes it by its destination address: to the Mac, to the host, or to
// both (broadcast and multicast). Frames from either go out unchanged.
//
// The Mac gets its own address, made from the host's by setting the
// "locally administered" bit (IEEE 802: bit 1 of the first byte), so each
// Pi gives its Mac a different one. Freestanding.

#ifndef G3_PLATFORM_COMMON_NET_SWITCH_H
#define G3_PLATFORM_COMMON_NET_SWITCH_H

#include "core/common/types.h"

namespace g3 {
namespace net_switch {

enum : u32 { kToMac = 1, kToHost = 2 };

// Where a received frame goes (0: nowhere).
u32 route(const u8* frame, u32 len, const u8 mac_address[6], const u8 host_address[6]);

// The Mac's address for a host address (differs from it, unicast, local).
void mac_address_for(const u8 host_address[6], u8 out[6]);

} // namespace net_switch
} // namespace g3

#endif // G3_PLATFORM_COMMON_NET_SWITCH_H
