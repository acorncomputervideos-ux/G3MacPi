// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/net_switch.cpp — see net_switch.h.

#include "platform/common/net_switch.h"

namespace g3 {
namespace net_switch {

namespace {
bool same(const u8* a, const u8* b)
{
    for (u32 i = 0; i < 6; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

u32 route(const u8* frame, u32 len, const u8 mac_address[6], const u8 host_address[6])
{
    if (len < 14)
        return 0;
    if (frame[0] & 1)  // group address: broadcast or multicast
        return kToMac | kToHost;
    if (same(frame, mac_address))
        return kToMac;
    if (same(frame, host_address))
        return kToHost;
    return 0;
}

void mac_address_for(const u8 host_address[6], u8 out[6])
{
    for (u32 i = 0; i < 6; ++i)
        out[i] = host_address[i];
    out[0] = static_cast<u8>((out[0] | 0x02) & ~0x01);  // locally administered, unicast
    if (out[0] == host_address[0])
        out[5] ^= 0x01;  // the host's was local already: differ elsewhere
}

} // namespace net_switch
} // namespace g3
