/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * guest/tests/kernels.c — test functions compiled twice: natively for the
 * host, and with the PowerPC cross-compiler for the emulated 750. The
 * guest-code test (tests/guest/run_kernels.cpp) runs each one on the
 * emulator and requires the same result as the native build. Freestanding:
 * no library calls.
 */
#include <stdint.h>

#include "guest/tests/kernels.h"

uint32_t k_fib(uint32_t n, uint32_t unused)
{
    (void)unused;
    uint32_t a = 0, b = 1;
    while (n--) {
        const uint32_t t = a + b;
        a = b;
        b = t;
    }
    return a;
}

uint32_t k_crc32(uint32_t seed, uint32_t len)
{
    /* CRC-32 (reflected, 0xEDB88320) over the bytes i * seed + 7. */
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= (uint8_t)(i * seed + 7);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

uint32_t k_sort_checksum(uint32_t seed, uint32_t n)
{
    /* Insertion sort of n pseudo-random words; returns a position-weighted sum. */
    static uint32_t a[256];
    if (n > 256)
        n = 256;
    uint32_t x = seed | 1;
    for (uint32_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        a[i] = x;
    }
    for (uint32_t i = 1; i < n; ++i) {
        const uint32_t v = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = v;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; ++i)
        sum += a[i] * (i + 1);
    return sum;
}

uint32_t k_signed_math(uint32_t a, uint32_t b)
{
    /* Signed division, remainder, shifts and comparisons. */
    const int32_t x = (int32_t)a, y = (int32_t)(b | 1);
    int32_t r = x / y + x % y;
    r ^= (x >> 3) + (int32_t)((uint32_t)y << 2);
    r += (x < y) ? 17 : -23;
    return (uint32_t)r;
}

uint32_t k_mul64(uint32_t a, uint32_t b)
{
    /* 64-bit multiply and adds, folded to 32 bits. */
    const uint64_t p = (uint64_t)a * b + ((uint64_t)b << 7);
    return (uint32_t)(p >> 32) ^ (uint32_t)p;
}

uint32_t k_float(uint32_t a, uint32_t b)
{
    /* Double-precision arithmetic: a polynomial, division, conversion. */
    const double x = (double)a / 1000.0, y = (double)(b % 1000 + 1) / 7.0;
    double r = ((x * 1.5 - y) * x + 0.25) / (y + 2.0);
    r = r * r - x / y;
    return (uint32_t)(int32_t)(r * 1000.0);
}

uint32_t k_single(uint32_t a, uint32_t b)
{
    /* Single precision, including a fused-style expression. */
    const float x = (float)(a & 0xFFFF) * 0.5f, y = (float)((b & 0xFF) + 1);
    const float r = x * y + x / y - (x - y) * 0.125f;
    return (uint32_t)(int32_t)r;
}

uint32_t k_strings(uint32_t seed, uint32_t n)
{
    /* Byte copies and comparisons through pointers into a buffer. */
    static char buf[128];
    if (n > 100)
        n = 100;
    for (uint32_t i = 0; i < n; ++i)
        buf[i] = (char)('a' + (seed + i * 7) % 26);
    buf[n] = 0;
    uint32_t h = 5381;
    for (const char* p = buf; *p; ++p)
        h = h * 33 + (uint8_t)*p;
    return h;
}

const kernel_fn k_table[K_COUNT] = {
    k_fib, k_crc32, k_sort_checksum, k_signed_math, k_mul64, k_float, k_single, k_strings,
};
