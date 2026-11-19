// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/machine/machine_config: the g3macpi.ini model.

#include "core/machine/machine_config.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <string>

using namespace g3;

namespace {
void parse(const std::string& text, MachineConfig& c, ConfigParseReport& r)
{
    parse_machine_config(text.data(), static_cast<u32>(text.size()), c, r);
}
} // namespace

TEST_CASE("machine/config/defaults")
{
    MachineConfig c;
    CHECK_EQ(c.ram_mb, 256u);
    CHECK(c.drive[0].type == MachineConfig::DriveType::CdRom);   // the B&W's CD position
    CHECK(c.drive[0].read_only);
    CHECK(c.drive[0].image[0] == 0);                             // an empty CD-ROM drive
    for (u32 i = 1; i < MachineConfig::kDrives; ++i)
        CHECK(c.drive[i].type == MachineConfig::DriveType::None);
    CHECK(c.boot == MachineConfig::Boot::Auto);
    CHECK(MachineConfig::valid_mode(c.width, c.height, c.depth));
}

TEST_CASE("machine/config/parse_full_file")
{
    const std::string text =
        "; a comment\n"
        "# another\n"
        "[Machine]\r\n"
        "  RAM_MB = 512   ; inline comment\n"
        "rom = /g3macpi/rom/Mac OS ROM 1.6\n"
        "boot = CDROM\n"
        "display = 1280x1024x32\n"
        "\n"
        "[ata0.master]\n"
        "type = cdrom\n"
        "image = /g3macpi/disks/Mac OS 9.iso\n"
        "[ata1.master]\n"
        "type = disk\n"
        "image = /g3macpi/disks/hd.img\n"
        "readonly = no\n"
        "[ata1.slave]\n"
        "type=disk\n"
        "image=/g3macpi/disks/data.img\n"
        "readonly=yes\n";
    MachineConfig c;
    ConfigParseReport r;
    parse(text, c, r);
    CHECK_EQ(r.ignored, 0u);
    CHECK_EQ(c.ram_mb, 512u);
    CHECK(std::strcmp(c.rom, "/g3macpi/rom/Mac OS ROM 1.6") == 0);
    CHECK(c.boot == MachineConfig::Boot::CdRom);
    CHECK_EQ(c.width, 1280u);
    CHECK_EQ(c.height, 1024u);
    CHECK_EQ(c.depth, 32u);
    CHECK(std::strcmp(c.drive[0].image, "/g3macpi/disks/Mac OS 9.iso") == 0);
    CHECK(c.drive[2].type == MachineConfig::DriveType::Disk);
    CHECK(!c.drive[2].read_only);
    CHECK(std::strcmp(c.drive[2].image, "/g3macpi/disks/hd.img") == 0);
    CHECK(c.drive[3].read_only);
    CHECK(c.drive[1].type == MachineConfig::DriveType::None);    // untouched
}

TEST_CASE("machine/config/invalid_lines_are_reported_and_ignored")
{
    const std::string text =
        "ram_mb = 128\n"              // 1: outside a section
        "[machine]\n"
        "ram_mb = 300\n"              // 3: not an offered size
        "display = 1000x700x8\n"      // 4: not an offered mode
        "display = 1024x768x24\n"     // 5: depth not offered
        "boot = floppy\n"             // 6
        "colour = blue\n"             // 7: unknown key
        "[ata0.master]\n"
        "type = tape\n"               // 9
        "readonly = maybe\n"          // 10
        "[ata7.master]\n"             // 11: unknown section
        "type = disk\n"               // (inside the unknown section: not counted again)
        "[floppy]\n"
        "image = /g3macpi/disks/sys.img\n"   // 14: no floppy drive
        "just text\n";                // 15: not key = value -> in [floppy], still rejected
    MachineConfig c;
    ConfigParseReport r;
    parse(text, c, r);
    CHECK_EQ(r.first_line, 1u);
    CHECK(std::strcmp(r.first_reason, "key outside a section") == 0);
    CHECK_EQ(r.ignored, 11u);
    CHECK(r.floppy);
    // Nothing invalid was applied.
    CHECK_EQ(c.ram_mb, 256u);
    CHECK_EQ(c.width, 1024u);
    CHECK_EQ(c.depth, 8u);
    CHECK(c.boot == MachineConfig::Boot::Auto);
    CHECK(c.drive[0].type == MachineConfig::DriveType::CdRom);
}

TEST_CASE("machine/config/cdrom_is_always_read_only")
{
    MachineConfig c;
    ConfigParseReport r;
    parse("[ata1.slave]\ntype = cdrom\nreadonly = no\n", c, r);
    CHECK(c.drive[3].type == MachineConfig::DriveType::CdRom);
    CHECK(c.drive[3].read_only);
}

TEST_CASE("machine/config/long_paths_are_refused")
{
    MachineConfig c;
    ConfigParseReport r;
    const std::string longpath(MachineConfig::kPathMax, 'a');   // one byte too many
    parse("[ata1.master]\nimage = " + longpath + "\n", c, r);
    CHECK_EQ(r.ignored, 1u);
    CHECK(c.drive[2].image[0] == 0);
    CHECK(!c.set_image(2, longpath.c_str()));
    CHECK(c.set_image(2, longpath.substr(1).c_str()));           // exactly fits
    CHECK_EQ(std::strlen(c.drive[2].image), MachineConfig::kPathMax - 1);
}

TEST_CASE("machine/config/write_then_parse_round_trips")
{
    MachineConfig a;
    a.ram_mb = 384;
    a.boot = MachineConfig::Boot::Disk;
    a.width = 800;
    a.height = 600;
    a.depth = 16;
    REQUIRE(a.set_image(0, "/g3macpi/disks/install.iso"));
    a.drive[2].type = MachineConfig::DriveType::Disk;
    REQUIRE(a.set_image(2, "/g3macpi/disks/Macintosh HD.img"));
    a.drive[2].read_only = true;
    char buf[2048];
    const u32 n = write_machine_config(a, buf, sizeof buf);
    REQUIRE(n > 0);
    CHECK_EQ(std::strlen(buf), n);

    MachineConfig b;
    ConfigParseReport r;
    parse_machine_config(buf, n, b, r);
    CHECK_EQ(r.ignored, 0u);
    CHECK_EQ(b.ram_mb, 384u);
    CHECK(b.boot == MachineConfig::Boot::Disk);
    CHECK_EQ(b.width, 800u);
    CHECK_EQ(b.depth, 16u);
    CHECK(std::strcmp(b.drive[0].image, "/g3macpi/disks/install.iso") == 0);
    CHECK(b.drive[2].type == MachineConfig::DriveType::Disk);
    CHECK(b.drive[2].read_only);
    CHECK(std::strcmp(b.drive[2].image, "/g3macpi/disks/Macintosh HD.img") == 0);
    CHECK(b.drive[1].type == MachineConfig::DriveType::None);

    // Too small a buffer: refused, not truncated.
    CHECK_EQ(write_machine_config(a, buf, 40), 0u);
}
