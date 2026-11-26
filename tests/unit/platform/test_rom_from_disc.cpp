// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_rom_from_disc.cpp — the Mac OS ROM file found on
// a disc image (the 'tbxi' file of the blessed folder, as Open Firmware
// does) and checked (a CHRP boot file), so a Mac OS CD supplies it.

#include "platform/common/rom_from_disc.h"
#include "core/storage/block_device.h"
#include "tests/framework/g3test.h"
#include "tests/unit/storage/hfs_image.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace g3;
using namespace hfs_image;

namespace {
// The test volume's ROM file, given the CHRP boot header a real one has.
void make_chrp(std::vector<u8>& img, const std::vector<u8>& rom)
{
    auto it = std::search(img.begin(), img.end(), rom.begin(), rom.begin() + 64);
    REQUIRE(it != img.end());
    std::memcpy(&*it, "<CHRP-BOOT>", 11);
}
} // namespace

TEST_CASE("platform/rom_from_disc/is_mac_os_rom")
{
    const char good[] = "<CHRP-BOOT>\r<COMPATIBLE>\rMacRISC";
    CHECK(is_mac_os_rom(reinterpret_cast<const u8*>(good), sizeof good));
    std::vector<u8> chip(4u << 20, 0);  // an old ROM-chip image: raw code
    chip[0] = 0x00;
    chip[1] = 0x0D;
    CHECK(!is_mac_os_rom(chip.data(), static_cast<u32>(chip.size())));
    CHECK(!is_mac_os_rom(reinterpret_cast<const u8*>(good), 5));
    CHECK(!is_mac_os_rom(nullptr, 100));
}

TEST_CASE("platform/rom_from_disc/found_in_the_system_folder")
{
    Volume vol;
    make_chrp(vol.img.d, vol.rom);
    RamBlockDevice dev(vol.img.d.data(), vol.img.d.size(), true);
    std::vector<u8> buf(64 * 1024, 0xEE);
    const u32 n = find_mac_os_rom(dev, buf.data(), static_cast<u32>(buf.size()));
    REQUIRE(n == Volume::kRomSize);
    CHECK(std::memcmp(buf.data(), "<CHRP-BOOT>", 11) == 0);
    CHECK(std::equal(vol.rom.begin() + 11, vol.rom.end(), buf.begin() + 11));
    // Too small a buffer: not found (nothing half-read is used).
    CHECK_EQ(find_mac_os_rom(dev, buf.data(), Volume::kRomSize - 1), 0u);
}

TEST_CASE("platform/rom_from_disc/not_a_rom_or_not_a_volume")
{
    // The tbxi file without the CHRP header: refused.
    Volume vol;
    RamBlockDevice dev(vol.img.d.data(), vol.img.d.size(), true);
    std::vector<u8> buf(64 * 1024);
    CHECK_EQ(find_mac_os_rom(dev, buf.data(), static_cast<u32>(buf.size())), 0u);
    // Not HFS at all (an ISO 9660-only disc, say).
    std::vector<u8> junk(1u << 20, 0x5A);
    RamBlockDevice none(junk.data(), junk.size(), true);
    CHECK_EQ(find_mac_os_rom(none, buf.data(), static_cast<u32>(buf.size())), 0u);
}
