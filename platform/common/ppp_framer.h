// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/ppp_framer.h — PPP in HDLC-like framing over a serial
// byte stream (RFC 1662), for the dial-up provider (docs/devices/modem.md).
//
//  - A frame: flag 0x7E, address 0xFF, control 0x03, protocol (2 bytes, or
//    1 when its value is odd and Protocol-Field-Compression was agreed),
//    information, FCS-16 (2 bytes, least significant first), flag. With
//    Address-and-Control-Field-Compression the 0xFF 0x03 may be left out.
//  - Transparency: 0x7E and 0x7D are sent as 0x7D, byte ^ 0x20; so are the
//    control characters (< 0x20) set in the async control character map
//    (ACCM; all of them until LCP agrees otherwise).
//  - FCS-16: polynomial x^16 + x^12 + x^5 + 1 (0x8408 reflected), initial
//    0xFFFF, sent complemented; over a good frame including its FCS the
//    value is 0xF0B8.
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_PPP_FRAMER_H
#define G3_PLATFORM_COMMON_PPP_FRAMER_H

#include "core/common/types.h"

namespace g3 {

namespace ppp {
u16 fcs16(u16 fcs, const u8* data, u32 len);
constexpr u16 kFcsInit = 0xFFFF, kFcsGood = 0xF0B8;
} // namespace ppp

class PppFramer {
public:
    static constexpr u32 kMaxInfo = 1600;  // above the default MRU (1500)

    // Received frames: protocol and information (address, control and FCS
    // removed). Frames with a bad FCS, too long or too short are dropped.
    using FrameFn = void (*)(void* context, u16 protocol, const u8* info, u32 len);
    void set_receiver(FrameFn fn, void* ctx) { rx_ = fn; rx_ctx_ = ctx; }
    void receive(u8 byte);
    u32 bad_frames() const { return bad_; }

    // Encodes one frame into out (room: 2 * (len + 6) + 2 bytes is enough);
    // returns the bytes written.
    struct Options {
        u32 accm = 0xFFFFFFFFu;  // control characters to escape when sending
        bool acfc = false;       // leave out 0xFF 0x03 (never for LCP)
        bool pfc = false;        // one-byte protocol when it allows
    };
    static u32 encode(u16 protocol, const u8* info, u32 len, const Options& opt, u8* out);

private:
    u8 buf_[kMaxInfo + 8] = {};
    u32 n_ = 0;
    bool escape_ = false, overflow_ = false;
    u32 bad_ = 0;
    FrameFn rx_ = nullptr;
    void* rx_ctx_ = nullptr;
    void end_frame();
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_PPP_FRAMER_H
