// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/machine_state.cpp — see machine_state.h.

#include "platform/common/machine_state.h"

namespace g3 {

namespace {

void put32(u8* p, u32 v)
{
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

u32 get32(const u8* p)
{
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

constexpr u32 kOffClock = 8, kOffNvram = 12, kOffPram = kOffNvram + MachineState::kNvramSize,
              kOffCrc = kOffPram + MachineState::kPramSize;

} // namespace

u32 crc32_ieee(const u8* data, u32 len)
{
    // Reflected polynomial 0xEDB88320, initial and final value ~0 (zlib's crc32).
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; ++i) {
        crc ^= data[i];
        for (u32 b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void MachineState::encode(u8* out) const
{
    out[0] = 'G';
    out[1] = '3';
    out[2] = 'S';
    out[3] = 'T';
    put32(out + 4, kVersion);
    put32(out + kOffClock, rtc_1904);
    for (u32 i = 0; i < kNvramSize; ++i)
        out[kOffNvram + i] = nvram[i];
    for (u32 i = 0; i < kPramSize; ++i)
        out[kOffPram + i] = pram[i];
    put32(out + kOffCrc, crc32_ieee(out, kOffCrc));
}

bool MachineState::decode(const u8* in, u32 len)
{
    if (!in || len != kFileSize || in[0] != 'G' || in[1] != '3' || in[2] != 'S' || in[3] != 'T')
        return false;
    if (get32(in + 4) != kVersion || get32(in + kOffCrc) != crc32_ieee(in, kOffCrc))
        return false;
    rtc_1904 = get32(in + kOffClock);
    for (u32 i = 0; i < kNvramSize; ++i)
        nvram[i] = in[kOffNvram + i];
    for (u32 i = 0; i < kPramSize; ++i)
        pram[i] = in[kOffPram + i];
    return true;
}

u64 choose_start_time(u64 host_unix, u64 trusted_from, const MachineState* saved, u64 fallback)
{
    if (host_unix >= trusted_from)
        return host_unix;
    if (saved && saved->rtc_1904 > kUnixTo1904)  // saved, and after 1970
        return saved->rtc_1904 - kUnixTo1904;
    return fallback;
}

} // namespace g3
