// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/common/cstring.h — the few C string helpers the freestanding core
// needs (no libc in core/).

#ifndef G3_CORE_COMMON_CSTRING_H
#define G3_CORE_COMMON_CSTRING_H

#include "core/common/types.h"

namespace g3::cstr {

constexpr u32 length(const char* s)
{
    u32 n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

constexpr bool equal(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

// Compares a (not NUL-terminated) range [a, a + n) with NUL-terminated b.
constexpr bool equal_n(const char* a, u32 n, const char* b)
{
    for (u32 i = 0; i < n; ++i)
        if (b[i] == '\0' || a[i] != b[i])
            return false;
    return b[n] == '\0';
}

inline void copy_bytes(void* dst, const void* src, u32 n)
{
    auto* d = static_cast<u8*>(dst);
    const auto* s = static_cast<const u8*>(src);
    for (u32 i = 0; i < n; ++i)
        d[i] = s[i];
}

} // namespace g3::cstr

#endif // G3_CORE_COMMON_CSTRING_H
