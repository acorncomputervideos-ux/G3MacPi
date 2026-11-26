// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/pi_overclock.cpp — see pi_overclock.h.

#include "platform/common/pi_overclock.h"

namespace g3::pi_overclock {

namespace {

constexpr char kBegin[] = "# >>> G3MacPi CPU speed";
constexpr char kEnd[] = "# <<< G3MacPi CPU speed";
constexpr char kBlock[] =
    "# >>> G3MacPi CPU speed (the settings menu writes this block; remove it to undo)\n"
    "[pi4]\n"
    "arm_freq=2000\n"
    "over_voltage=6\n"
    "[all]\n"
    "# <<< G3MacPi CPU speed\n";

bool starts(const char* p, u32 n, const char* prefix)
{
    u32 i = 0;
    for (; prefix[i]; ++i)
        if (i >= n || p[i] != prefix[i])
            return false;
    return true;
}

// The line starting at `at`: its end (past the newline).
u32 line_end(const char* t, u32 len, u32 at)
{
    while (at < len && t[at] != '\n')
        ++at;
    return at < len ? at + 1 : at;
}

} // namespace

bool present(const char* t, u32 len)
{
    for (u32 at = 0; at < len; at = line_end(t, len, at))
        if (starts(t + at, len - at, kBegin))
            return true;
    return false;
}

u32 apply(const char* t, u32 len, bool on, char* out, u32 cap, bool& changed)
{
    u32 n = 0;
    bool ok = true;
    auto put = [&](char c) {
        if (n < cap)
            out[n++] = c;
        else
            ok = false;
    };
    // Copy every line outside our block(s).
    bool inside = false;
    for (u32 at = 0; at < len;) {
        const u32 end = line_end(t, len, at);
        if (!inside && starts(t + at, len - at, kBegin)) {
            inside = true;
        } else if (inside) {
            if (starts(t + at, len - at, kEnd))
                inside = false;
        } else {
            for (u32 i = at; i < end; ++i)
                put(t[i]);
        }
        at = end;
    }
    if (on) {
        if (n > 0 && out[n - 1] != '\n')
            put('\n');
        for (const char* p = kBlock; *p; ++p)
            put(*p);
    }
    if (!ok)
        return 0;
    changed = n != len;
    for (u32 i = 0; !changed && i < n; ++i)
        changed = out[i] != t[i];
    return n;
}

} // namespace g3::pi_overclock
