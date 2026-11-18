/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/smoke/smoke.c — checks that the PowerPC cross-compiler can build
 * freestanding code for the emulated G3 (the way our Mac OS display
 * driver will be built). It is compiled in CI only; nothing runs it yet.
 */
typedef unsigned int u32;

u32 g3_checksum(const unsigned char* p, u32 n)
{
    u32 sum = 0;
    for (u32 i = 0; i < n; ++i)
        sum = (sum << 1 | sum >> 31) ^ p[i];
    return sum;
}
