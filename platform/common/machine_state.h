// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/machine_state.h — what the Mac keeps across power-off on
// a real machine: the Cuda's real-time clock and parameter RAM, and the
// 8 KB NVRAM (Open Firmware's settings and Mac OS's XPRAM partition, which
// holds the time zone, the start-up disk and similar). Front ends save it
// to a file and give it back at the next start.
//
// File format (ours): "G3ST", a big-endian u32 version (1), the clock
// (u32, seconds since 1904-01-01), the NVRAM (8192 bytes), the PRAM (256
// bytes), then a big-endian CRC-32 (IEEE 802.3, as zlib) of everything
// before it. Any other size, version or checksum is rejected.
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_MACHINE_STATE_H
#define G3_PLATFORM_COMMON_MACHINE_STATE_H

#include "core/common/types.h"

namespace g3 {

struct MachineState {
    static constexpr u32 kNvramSize = 0x2000;
    static constexpr u32 kPramSize = 256;
    static constexpr u32 kVersion = 1;
    static constexpr u32 kFileSize = 4 + 4 + 4 + kNvramSize + kPramSize + 4;

    u32 rtc_1904 = 0;  // 0: no clock saved
    u8 nvram[kNvramSize] = {};
    u8 pram[kPramSize] = {};

    // Writes kFileSize bytes to `out`.
    void encode(u8* out) const;
    // False (and the state unchanged) if `in` is not a valid state file.
    bool decode(const u8* in, u32 len);
};

u32 crc32_ieee(const u8* data, u32 len);

// Seconds from 1904-01-01 (the Mac's epoch) to 1970-01-01 (Unix's).
constexpr u64 kUnixTo1904 = 2082844800ull;

// The clock to start the Mac with: the host's time when it is known (on or
// after `trusted_from`, Unix seconds), otherwise the saved clock if there
// is one, otherwise `fallback` (Unix seconds). Returns Unix seconds.
u64 choose_start_time(u64 host_unix, u64 trusted_from, const MachineState* saved, u64 fallback);

} // namespace g3

#endif // G3_PLATFORM_COMMON_MACHINE_STATE_H
