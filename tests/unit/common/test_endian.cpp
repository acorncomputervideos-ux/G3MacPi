// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for core/common/endian.h

#include "core/common/endian.h"
#include "tests/framework/g3test.h"

using namespace g3;

TEST_CASE("common/endian/bswap")
{
    CHECK_EQ(byteswap16(0x1234u), 0x3412u);
    CHECK_EQ(byteswap32(0x12345678u), 0x78563412u);
    CHECK_EQ(byteswap64(0x0123456789ABCDEFull), 0xEFCDAB8967452301ull);
    CHECK_EQ(byteswap32(byteswap32(0xDEADBEEFu)), 0xDEADBEEFu);
}

TEST_CASE("common/endian/load_be")
{
    const u8 bytes[9] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFF};
    CHECK_EQ(load_be16(bytes), 0x0123u);
    CHECK_EQ(load_be32(bytes), 0x01234567u);
    CHECK_EQ(load_be64(bytes), 0x0123456789ABCDEFull);
    // Unaligned access must work (guest code performs unaligned loads).
    CHECK_EQ(load_be32(bytes + 1), 0x23456789u);
    CHECK_EQ(load_be16(bytes + 7), 0xEFFFu);
}

TEST_CASE("common/endian/store_be")
{
    u8 buf[9] = {};
    store_be32(buf + 1, 0xCAFEBABEu);
    CHECK_EQ(buf[0], 0x00u);
    CHECK_EQ(buf[1], 0xCAu);
    CHECK_EQ(buf[2], 0xFEu);
    CHECK_EQ(buf[3], 0xBAu);
    CHECK_EQ(buf[4], 0xBEu);
    CHECK_EQ(buf[5], 0x00u);

    store_be16(buf, 0xA1B2u);
    CHECK_EQ(buf[0], 0xA1u);
    CHECK_EQ(buf[1], 0xB2u);

    store_be64(buf, 0x1122334455667788ull);
    CHECK_EQ(load_be64(buf), 0x1122334455667788ull);
    CHECK_EQ(buf[0], 0x11u);
    CHECK_EQ(buf[7], 0x88u);
}

TEST_CASE("common/endian/little_endian_access")
{
    u8 buf[4] = {0x78, 0x56, 0x34, 0x12};
    CHECK_EQ(load_le32(buf), 0x12345678u);
    CHECK_EQ(load_le16(buf), 0x5678u);
    store_le32(buf, 0xA0B0C0D0u);
    CHECK_EQ(buf[0], 0xD0u);
    CHECK_EQ(buf[3], 0xA0u);
    store_le16(buf, 0x1234u);
    CHECK_EQ(buf[0], 0x34u);
    CHECK_EQ(buf[1], 0x12u);
}

TEST_CASE("common/endian/be_le_are_mirror_images")
{
    u8 buf[4];
    store_be32(buf, 0x01020304u);
    CHECK_EQ(load_le32(buf), 0x04030201u);
}
