/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef G3_GUEST_TESTS_KERNELS_H
#define G3_GUEST_TESTS_KERNELS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*kernel_fn)(uint32_t, uint32_t);
enum { K_COUNT = 8 };
extern const kernel_fn k_table[K_COUNT];

/* The guest's command block (fixed address, see start.S). */
#define K_CMD_ADDR 0x00008000u   /* +0 kernel number, +4 a, +8 b, +12 result */

#ifdef __cplusplus
}
#endif
#endif
