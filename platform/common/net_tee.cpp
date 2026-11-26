// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/net_tee.cpp — see net_tee.h.

#include "platform/common/net_tee.h"

namespace g3 {

void NetTee::pump()
{
    if (!host_)
        return;
    for (u32 guard = 0; guard < 4 * kFrames; ++guard) {
        Slot& s = q_[(head_ + n_) % kFrames];
        const u32 len = host_->net_receive(s.data);
        if (len == 0 || len > kMaxFrame)
            return;
        s.len = len;
        if (seen_)
            seen_(seen_ctx_, s.data, len);
        if (n_ == kFrames)
            head_ = (head_ + 1) % kFrames;  // full: the oldest goes
        else
            ++n_;
    }
}

u32 NetTee::net_receive(u8* buf)
{
    pump();
    if (n_ == 0)
        return 0;
    const Slot& s = q_[head_];
    for (u32 i = 0; i < s.len; ++i)
        buf[i] = s.data[i];
    head_ = (head_ + 1) % kFrames;
    --n_;
    return s.len;
}

} // namespace g3
