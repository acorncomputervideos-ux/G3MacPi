// SPDX-License-Identifier: GPL-3.0-or-later
// The B&W NVRAM layout: CHRP partition headers, checksums, the chain, and
// the offsets Mac OS and Linux expect (bw_nvram.h).

#include "core/machine/bw_firmware.h"
#include "core/machine/bw_nvram.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace g3;

TEST_CASE("machine/bw-nvram/checksum")
{
    // Worked by hand: 0x50 + 0x00 0x80 + "common" = 0x50+0x80+0x63+0x6F+0x6D+0x6D+0x6F+0x6E
    // = 0x359 -> 0x59 + 0x03 = 0x5C.
    u8 h[16] = {0x50, 0, 0x00, 0x80, 'c', 'o', 'm', 'm', 'o', 'n'};
    CHECK_EQ(bw_nvram::checksum(h), 0x5Cu);
    // A carry that needs two folds: 0xFF * 15 = 0xEF1 -> 0xF1 + 0x0E = 0xFF.
    u8 f[16];
    std::memset(f, 0xFF, 16);
    CHECK_EQ(bw_nvram::checksum(f), 0xFFu);
}

TEST_CASE("machine/bw-nvram/format_and_walk")
{
    MacioNvram nv;
    CHECK(!bw_nvram::valid(nv));                       // blank: lengths 0
    bw_nvram::format(nv);
    REQUIRE(bw_nvram::valid(nv));
    NvramPartition p{};
    REQUIRE(bw_nvram::partition(nv, 0, p));
    CHECK(p.offset == 0 && p.signature == 0x7F && p.length == 0x130);
    CHECK(std::string(p.name, 12) == "wwwwwwwwwwww");
    REQUIRE(bw_nvram::partition(nv, 1, p));
    CHECK(p.offset == 0x130 && p.signature == 0x50 && std::string(p.name) == "common");
    REQUIRE(bw_nvram::partition(nv, 2, p));
    CHECK(p.offset == 0x11A0 && p.signature == 0xA0 && std::string(p.name) == "APL,MacOS75");
    CHECK(p.length == 0x510u);
    REQUIRE(bw_nvram::partition(nv, 3, p));
    CHECK(p.offset == 0x16B0 && p.signature == 0x7F && p.offset + p.length == MacioNvram::kSize);
    CHECK(!bw_nvram::partition(nv, 4, p));
    // Walk exactly as Linux's lookup_partitions does.
    u32 offset = 0, xpram = 0, nr = 0, common = 0;
    do {
        char name[13] = {};
        for (u32 i = 0; i < 12; ++i)
            name[i] = static_cast<char>(nv.byte(offset + 4 + i));
        if (!std::strcmp(name, "common"))
            common = offset + 0x10;
        if (!std::strcmp(name, "APL,MacOS75")) {
            xpram = offset + 0x10;
            nr = offset + 0x110;
        }
        offset += ((static_cast<u32>(nv.byte(offset + 2)) << 8) | nv.byte(offset + 3)) * 0x10;
    } while (offset < MacioNvram::kSize);
    // What Linux reported on a real B&W: OF 0x140, XP 0x11b0, NR 0x12b0.
    CHECK_EQ(common, 0x140u);
    CHECK_EQ(xpram, 0x11B0u);
    CHECK_EQ(nr, 0x12B0u);
    // Damage is detected.
    nv.set_byte(0x130 + 5, 'X');
    CHECK(!bw_nvram::valid(nv));
}

TEST_CASE("machine/bw-nvram/firmware_formats_blank_nvram_only")
{
    constexpr u32 kRam = 16u << 20;
    std::vector<u8> ram(kRam);
    YosemiteConfig cfg;
    cfg.ram = ram.data();
    cfg.ram_size = kRam;
    {
        YosemiteMachine m(cfg);
        BwFirmware fw(m, BwFirmware::Config{});
        REQUIRE(fw.ok());
        CHECK(bw_nvram::valid(m.macio().nvram()));
    }
    {
        // A valid NVRAM with data in it is left alone.
        YosemiteMachine m(cfg);
        bw_nvram::format(m.macio().nvram());
        m.macio().nvram().set_byte(0x11B5, 0xAB);      // XPRAM data
        BwFirmware fw(m, BwFirmware::Config{});
        REQUIRE(fw.ok());
        CHECK(m.macio().nvram().byte(0x11B5) == 0xAB);
    }
}
