// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/ppp_framer.cpp — see ppp_framer.h.

#include "platform/common/ppp_framer.h"

namespace g3 {

u16 ppp::fcs16(u16 fcs, const u8* data, u32 len)
{
    for (u32 i = 0; i < len; ++i) {
        fcs ^= data[i];
        for (u32 b = 0; b < 8; ++b)
            fcs = (fcs & 1) ? static_cast<u16>((fcs >> 1) ^ 0x8408) : static_cast<u16>(fcs >> 1);
    }
    return fcs;
}

void PppFramer::receive(u8 byte)
{
    if (byte == 0x7E) {
        end_frame();
        return;
    }
    if (byte == 0x7D) {
        escape_ = true;
        return;
    }
    if (escape_) {
        byte ^= 0x20;
        escape_ = false;
    }
    if (n_ < sizeof buf_)
        buf_[n_++] = byte;
    else
        overflow_ = true;
}

void PppFramer::end_frame()
{
    const u32 n = n_;
    const bool overflow = overflow_;
    n_ = 0;
    escape_ = overflow_ = false;
    if (n == 0)
        return;  // back-to-back flags
    if (overflow || n < 4 || ppp::fcs16(ppp::kFcsInit, buf_, n) != ppp::kFcsGood) {
        ++bad_;
        return;
    }
    u32 i = 0;
    const u32 end = n - 2;  // without the FCS
    if (end - i >= 2 && buf_[0] == 0xFF && buf_[1] == 0x03)
        i = 2;
    if (i >= end) {
        ++bad_;
        return;
    }
    u16 protocol = buf_[i++];
    if (!(protocol & 1)) {  // two bytes (the first is even)
        if (i >= end) {
            ++bad_;
            return;
        }
        protocol = static_cast<u16>(protocol << 8 | buf_[i++]);
    }
    if (rx_)
        rx_(rx_ctx_, protocol, buf_ + i, end - i);
}

u32 PppFramer::encode(u16 protocol, const u8* info, u32 len, const Options& opt, u8* out)
{
    u8 head[4];
    u32 h = 0;
    const bool lcp = protocol == 0xC021;
    if (!opt.acfc || lcp) {
        head[h++] = 0xFF;
        head[h++] = 0x03;
    }
    if (opt.pfc && !lcp && protocol < 0x100 && (protocol & 1))
        head[h++] = static_cast<u8>(protocol);
    else {
        head[h++] = static_cast<u8>(protocol >> 8);
        head[h++] = static_cast<u8>(protocol);
    }
    // LCP is always sent with every control character escaped (RFC 1662 7.1).
    const u32 accm = lcp ? 0xFFFFFFFFu : opt.accm;
    u32 o = 0;
    auto put = [&](u8 b) {
        if (b == 0x7E || b == 0x7D || (b < 0x20 && ((accm >> b) & 1))) {
            out[o++] = 0x7D;
            out[o++] = static_cast<u8>(b ^ 0x20);
        } else {
            out[o++] = b;
        }
    };
    out[o++] = 0x7E;
    u16 fcs = ppp::fcs16(ppp::kFcsInit, head, h);
    fcs = ppp::fcs16(fcs, info, len);
    fcs = static_cast<u16>(~fcs);
    for (u32 i = 0; i < h; ++i)
        put(head[i]);
    for (u32 i = 0; i < len; ++i)
        put(info[i]);
    put(static_cast<u8>(fcs));
    put(static_cast<u8>(fcs >> 8));
    out[o++] = 0x7E;
    return o;
}

} // namespace g3
