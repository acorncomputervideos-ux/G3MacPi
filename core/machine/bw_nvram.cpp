// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/bw_nvram.cpp — see bw_nvram.h.

#include "core/machine/bw_nvram.h"

namespace g3::bw_nvram {

u8 checksum(const u8* h)
{
    u32 sum = h[0];
    for (u32 i = 2; i < 16; ++i)
        sum += h[i];
    while (sum > 0xFF)
        sum = (sum & 0xFF) + (sum >> 8);
    return static_cast<u8>(sum);
}

namespace {

void header_at(const MacioNvram& nv, u32 at, u8* h)
{
    for (u32 i = 0; i < 16; ++i)
        h[i] = nv.byte(at + i);
}

void write_header(MacioNvram& nv, u32 at, u8 sig, u32 length, const char* name, char pad)
{
    u8 h[16] = {};
    h[0] = sig;
    h[2] = static_cast<u8>((length / 16) >> 8);
    h[3] = static_cast<u8>(length / 16);
    u32 i = 0;
    for (; name[i] && i < 12; ++i)
        h[4 + i] = static_cast<u8>(name[i]);
    for (; i < 12; ++i)
        h[4 + i] = static_cast<u8>(pad);
    h[1] = checksum(h);
    for (u32 k = 0; k < 16; ++k)
        nv.set_byte(at + k, h[k]);
}

} // namespace

bool valid(const MacioNvram& nv)
{
    u32 at = 0;
    while (at < MacioNvram::kSize) {
        u8 h[16];
        header_at(nv, at, h);
        const u32 len = ((static_cast<u32>(h[2]) << 8) | h[3]) * 16;
        if (len < 16 || h[1] != checksum(h) || at + len > MacioNvram::kSize)
            return false;
        at += len;
    }
    return at == MacioNvram::kSize;
}

bool partition(const MacioNvram& nv, u32 index, NvramPartition& out)
{
    u32 at = 0;
    for (u32 i = 0; at < MacioNvram::kSize; ++i) {
        u8 h[16];
        header_at(nv, at, h);
        const u32 len = ((static_cast<u32>(h[2]) << 8) | h[3]) * 16;
        if (len < 16)
            return false;
        if (i == index) {
            out.offset = at;
            out.signature = h[0];
            out.length = len;
            for (u32 k = 0; k < 12; ++k)
                out.name[k] = static_cast<char>(h[4 + k]);
            out.name[12] = '\0';
            return true;
        }
        at += len;
    }
    return false;
}

void format(MacioNvram& nv)
{
    for (u32 i = 0; i < MacioNvram::kSize; ++i)
        nv.set_byte(i, 0);
    write_header(nv, 0, kSigFree, kMacOsHeader, "", 'w');
    write_header(nv, kMacOsHeader, kSigMacOs, kMacOsLength, "APL,MacOS75", '\0');
    write_header(nv, kCommonHeader, kSigOf, kCommonLength, "common", '\0');
}

} // namespace g3::bw_nvram
