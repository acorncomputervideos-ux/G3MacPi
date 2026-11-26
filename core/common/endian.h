// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/common/endian.h — byte-order helpers.
//
// The guest (PowerPC Macintosh) is big-endian; the host (Cortex-A72 in
// AArch64 mode, or x86-64 for tests) is little-endian. Guest RAM is stored in
// guest byte order, so every guest memory access goes through load_be*/store_be*.
// On AArch64 these compile to a load/store plus a single REV instruction.
//
// The functions are written to be correct on a big-endian host as well.

#ifndef G3_CORE_COMMON_ENDIAN_H
#define G3_CORE_COMMON_ENDIAN_H

#include "core/common/types.h"

namespace g3 {

constexpr bool host_is_little_endian()
{
    return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
}

// Named byteswap*, not bswap*: Circle's <circle/util.h> defines bswap16 and
// bswap32 as macros, which would silently rename these functions (found by
// -Winfinite-recursion on 2026-11-16).
constexpr u16 byteswap16(u16 v) { return __builtin_bswap16(v); }
constexpr u32 byteswap32(u32 v) { return __builtin_bswap32(v); }
constexpr u64 byteswap64(u64 v) { return __builtin_bswap64(v); }

// Convert between host order and big-endian order (the operation is symmetric).
constexpr u16 host_to_be16(u16 v) { return host_is_little_endian() ? byteswap16(v) : v; }
constexpr u32 host_to_be32(u32 v) { return host_is_little_endian() ? byteswap32(v) : v; }
constexpr u64 host_to_be64(u64 v) { return host_is_little_endian() ? byteswap64(v) : v; }
constexpr u16 be_to_host16(u16 v) { return host_to_be16(v); }
constexpr u32 be_to_host32(u32 v) { return host_to_be32(v); }
constexpr u64 be_to_host64(u64 v) { return host_to_be64(v); }

// Unaligned-safe loads/stores of big-endian values from/to a byte buffer.
// __builtin_memcpy avoids any dependency on <string.h> and is optimised into
// a single load/store by GCC/Clang.
inline u16 load_be16(const void* p) { u16 v; __builtin_memcpy(&v, p, 2); return be_to_host16(v); }
inline u32 load_be32(const void* p) { u32 v; __builtin_memcpy(&v, p, 4); return be_to_host32(v); }
inline u64 load_be64(const void* p) { u64 v; __builtin_memcpy(&v, p, 8); return be_to_host64(v); }

inline void store_be16(void* p, u16 v) { v = host_to_be16(v); __builtin_memcpy(p, &v, 2); }
inline void store_be32(void* p, u32 v) { v = host_to_be32(v); __builtin_memcpy(p, &v, 4); }
inline void store_be64(void* p, u64 v) { v = host_to_be64(v); __builtin_memcpy(p, &v, 8); }

// Little-endian variants: needed for PowerPC byte-reversed loads/stores
// (lhbrx/lwbrx/sthbrx/stwbrx) and for little-endian PCI devices.
inline u16 load_le16(const void* p) { u16 v; __builtin_memcpy(&v, p, 2); return host_is_little_endian() ? v : byteswap16(v); }
inline u32 load_le32(const void* p) { u32 v; __builtin_memcpy(&v, p, 4); return host_is_little_endian() ? v : byteswap32(v); }
inline void store_le16(void* p, u16 v) { if (!host_is_little_endian()) v = byteswap16(v); __builtin_memcpy(p, &v, 2); }
inline void store_le32(void* p, u32 v) { if (!host_is_little_endian()) v = byteswap32(v); __builtin_memcpy(p, &v, 4); }

} // namespace g3

#endif // G3_CORE_COMMON_ENDIAN_H
