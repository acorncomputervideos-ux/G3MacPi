// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/net_tee.h — the host's network link shared by the Mac's
// Ethernet card and the dial-up provider (docs/devices/modem.md). Both
// send frames out unchanged. Frames coming in are pulled from the host by
// pump() (the Ethernet card only pulls while its receiver is on), shown to
// the provider, and queued for the card; when the card does not take them,
// the oldest are dropped.
//
// Freestanding; one thread (the emulation core).

#ifndef G3_PLATFORM_COMMON_NET_TEE_H
#define G3_PLATFORM_COMMON_NET_TEE_H

#include "core/common/types.h"
#include "core/devices/macio/bmac.h"

namespace g3 {

class NetTee final : public NetLink {
public:
    static constexpr u32 kFrames = 32;
    using SeenFn = void (*)(void* context, const u8* frame, u32 len);

    explicit NetTee(NetLink* host) : host_(host) {}
    void set_watcher(SeenFn fn, void* ctx) { seen_ = fn; seen_ctx_ = ctx; }

    // The Ethernet card's side.
    void net_send(const u8* frame, u32 len) override { if (host_) host_->net_send(frame, len); }
    u32 net_receive(u8* buf) override;

    // The provider's side.
    void send(const u8* frame, u32 len) { if (host_) host_->net_send(frame, len); }
    void pump();  // takes what the host has

private:
    struct Slot { u32 len; u8 data[kMaxFrame]; };
    NetLink* host_;
    Slot q_[kFrames];
    u32 head_ = 0, n_ = 0;
    SeenFn seen_ = nullptr;
    void* seen_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_NET_TEE_H
