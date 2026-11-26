// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/net_bridge.h — Ethernet frames between the emulation core
// (the Mac's BMAC+) and the I/O core (the host's network device).
//
// The emulation core uses the NetLink side (core/devices/macio/bmac.h):
// net_send() queues a frame for the wire, net_receive() takes one that came
// in. The I/O core moves frames between these queues and its device:
// take_outgoing() and put_incoming(). Full queues drop frames (counted), as
// a busy network would. One core on each side; lock-free. Freestanding.

#ifndef G3_PLATFORM_COMMON_NET_BRIDGE_H
#define G3_PLATFORM_COMMON_NET_BRIDGE_H

#include "core/common/spsc_queue.h"
#include "core/common/types.h"
#include "core/devices/macio/bmac.h"

namespace g3 {

class NetBridge final : public NetLink {
public:
    static constexpr u32 kFrames = 32;  // each way

    struct Stats {
        u64 out = 0, out_dropped = 0, in = 0, in_dropped = 0;
    };

    // Emulation core.
    void net_send(const u8* frame, u32 len) override;
    u32 net_receive(u8* buf) override;

    // I/O core. take_outgoing: a frame for the wire into buf (kMaxFrame
    // bytes), its length, 0 when none. put_incoming: false when full.
    u32 take_outgoing(u8* buf);
    bool put_incoming(const u8* frame, u32 len);

    const Stats& stats() const { return st_; }

private:
    struct Frame {
        u32 len;
        u8 data[NetLink::kMaxFrame];
    };
    SpscQueue<Frame, kFrames> out_, in_;
    Stats st_;
    Frame tmp_emu_ = {}, tmp_io_ = {};  // each core copies through its own
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_NET_BRIDGE_H
