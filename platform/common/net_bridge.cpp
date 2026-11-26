// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/net_bridge.cpp — see net_bridge.h.

#include "platform/common/net_bridge.h"

namespace g3 {

namespace {
void copy(u8* d, const u8* s, u32 n)
{
    for (u32 i = 0; i < n; ++i)
        d[i] = s[i];
}
} // namespace

void NetBridge::net_send(const u8* frame, u32 len)
{
    if (len == 0 || len > NetLink::kMaxFrame) {
        ++st_.out_dropped;
        return;
    }
    tmp_emu_.len = len;
    copy(tmp_emu_.data, frame, len);
    if (out_.push(tmp_emu_))
        ++st_.out;
    else
        ++st_.out_dropped;
}

u32 NetBridge::net_receive(u8* buf)
{
    if (!in_.pop(tmp_emu_))
        return 0;
    copy(buf, tmp_emu_.data, tmp_emu_.len);
    return tmp_emu_.len;
}

u32 NetBridge::take_outgoing(u8* buf)
{
    if (!out_.pop(tmp_io_))
        return 0;
    copy(buf, tmp_io_.data, tmp_io_.len);
    return tmp_io_.len;
}

bool NetBridge::put_incoming(const u8* frame, u32 len)
{
    if (len == 0 || len > NetLink::kMaxFrame) {
        ++st_.in_dropped;
        return false;
    }
    tmp_io_.len = len;
    copy(tmp_io_.data, frame, len);
    if (!in_.push(tmp_io_)) {
        ++st_.in_dropped;
        return false;
    }
    ++st_.in;
    return true;
}

} // namespace g3
