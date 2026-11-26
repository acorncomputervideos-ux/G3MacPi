// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/devices/le_register.h.

#include "core/devices/le_register.h"
#include "tests/framework/g3test.h"

#include <initializer_list>

using namespace g3;

TEST_CASE("devices/le_register/read_lanes")
{
    const u32 reg = 0x44332211u;  // byte 0 = 0x11 at offset 0
    CHECK_EQ(le_reg_read(reg, 0, 4), 0x11223344u);  // lwz sees it byte-swapped
    CHECK_EQ(le_reg_read(reg, 0, 1), 0x11u);
    CHECK_EQ(le_reg_read(reg, 3, 1), 0x44u);
    CHECK_EQ(le_reg_read(reg, 0, 2), 0x1122u);
    CHECK_EQ(le_reg_read(reg, 2, 2), 0x3344u);
}

TEST_CASE("devices/le_register/write_lanes")
{
    CHECK_EQ(le_reg_write(0, 0, 4, 0x11223344u), 0x44332211u);
    CHECK_EQ(le_reg_write(0xFFFFFFFFu, 1, 1, 0x00), 0xFFFF00FFu);
    CHECK_EQ(le_reg_write(0, 2, 2, 0xABCD), 0xCDAB0000u);
    // Round trip for every size/offset combination.
    for (u32 size : {1u, 2u, 4u})
        for (u32 off = 0; off + size <= 4; off += size)
            CHECK_EQ(le_reg_read(le_reg_write(0, off, size, 0x89ABCDEFu & (size == 4 ? ~0u : (1u << (8 * size)) - 1)), off, size),
                     0x89ABCDEFu & (size == 4 ? ~0u : (1u << (8 * size)) - 1));
}
