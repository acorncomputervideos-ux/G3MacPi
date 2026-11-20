// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the CHRP bootinfo parser (CHRP binding rev 1.7, 3.1.6).

#include "core/firmware/openfirm/bootinfo.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <memory>
#include <string>

using namespace g3;
using namespace g3::of;

namespace {
std::unique_ptr<BootInfo> parse(const std::string& s, const BootInfoPath& p = {}, u32 index = 0,
                                bool* ok = nullptr)
{
    auto bi = std::make_unique<BootInfo>();
    const bool r = parse_bootinfo(reinterpret_cast<const u8*>(s.data()), static_cast<u32>(s.size()),
                                  p, *bi, index);
    if (ok)
        *ok = r;
    return bi;
}
} // namespace

TEST_CASE("firmware/bootinfo/full_object")
{
    BootInfoPath path;
    path.device = "/pci/mac-io/ata/disk@0";
    path.partition = "9";
    path.directory = "System Folder";
    path.filename = "Mac OS ROM";
    std::string s =
        "<CHRP-BOOT>\n"
        "<Compatible>MacRISC MacRISC2</Compatible>\n"          // not a CHRP tag: skipped
        "<description>Mac OS &amp; friends &lt;test&gt;</description>\n"
        "<OS-NAME>Mac OS</os-name>\n"
        "<boot-script>\n"
        "\" &device;:&partition;\" type\n"
        "&FULL-PATH; 1 2 &lt; .\n"
        "</BOOT-SCRIPT>\n"
        "<ICON SIZE=2,2 COLOR-SPACE=3,3,2><BITMAP>00 ff\n 1A b3</BITMAP></ICON>\n"
        "</chrp-boot>";
    const size_t body = s.size();
    s += std::string("\x04\x00\xFF binary data", 15);
    bool ok = false;
    auto bi = parse(s, path, 0, &ok);
    REQUIRE(ok);
    CHECK(std::string(bi->description) == "Mac OS & friends <test>");
    CHECK(std::string(bi->os_name) == "Mac OS");
    REQUIRE(bi->has_script);
    CHECK(std::string(bi->script) ==
          "\n\" /pci/mac-io/ata/disk@0:9\" type\n"
          "/pci/mac-io/ata/disk@0:9,\\System Folder\\Mac OS ROM 1 2 < .\n");
    CHECK_EQ(bi->script_len, static_cast<u32>(std::strlen(bi->script)));
    REQUIRE(bi->has_icon);
    CHECK_EQ(bi->icon_width, 2u);
    CHECK_EQ(bi->icon_height, 2u);
    CHECK_EQ(bi->icon_pixels, 4u);
    CHECK_EQ(bi->icon[0], 0x00u);
    CHECK_EQ(bi->icon[1], 0xFFu);
    CHECK_EQ(bi->icon[2], 0x1Au);
    CHECK_EQ(bi->icon[3], 0xB3u);
    CHECK_EQ(bi->body_offset, static_cast<u32>(body));
    CHECK(is_bootinfo(reinterpret_cast<const u8*>(s.data()), static_cast<u32>(s.size())));
}

TEST_CASE("firmware/bootinfo/defaults_entities_and_sections")
{
    // Icon defaults; path entities only inside BOOT-SCRIPT; unknown entity kept;
    // full-path without a partition.
    BootInfoPath path;
    path.device = "cd";
    path.directory = "ppc";
    path.filename = "bootinfo.txt";
    const std::string s =
        "  \n<chrp-boot><description>&device; &bogus; &amp</description>"
        "<boot-script>&full-path;</boot-script><icon><bitmap>01</bitmap></icon></chrp-boot>"
        "<CHRP-BOOT><OS-NAME>Second</OS-NAME></CHRP-BOOT>";
    bool ok = false;
    auto bi = parse(s, path, 0, &ok);
    REQUIRE(ok);
    CHECK(std::string(bi->description) == "&device; &bogus; &amp");
    CHECK(std::string(bi->script) == "cd:\\ppc\\bootinfo.txt");
    CHECK_EQ(bi->icon_width, 64u);
    CHECK_EQ(bi->icon_height, 64u);
    CHECK_EQ(bi->icon_bits[0], 3u);
    CHECK_EQ(bi->icon_bits[2], 2u);
    CHECK_EQ(bi->icon_pixels, 1u);
    auto second = parse(s, path, 1, &ok);
    REQUIRE(ok);
    CHECK(std::string(second->os_name) == "Second");
    CHECK(!second->has_script);
    parse(s, path, 2, &ok);
    CHECK(!ok);
    CHECK(is_bootinfo(reinterpret_cast<const u8*>(s.data()), static_cast<u32>(s.size())));
    const std::string not_bi = "\\ a Forth file";
    CHECK(!is_bootinfo(reinterpret_cast<const u8*>(not_bi.data()), static_cast<u32>(not_bi.size())));
}

TEST_CASE("firmware/bootinfo/malformed")
{
    bool ok = true;
    parse("<chrp-boot><os-name>x</os-name>", {}, 0, &ok);                 // no end tag
    CHECK(!ok);
    parse("<chrp-boot><boot-script>abc</chrp-boot>", {}, 0, &ok);        // unterminated element
    CHECK(!ok);
    parse("<chrp-boot></os-name></chrp-boot>", {}, 0, &ok);              // stray end tag
    CHECK(!ok);
    parse("<chrp-boot><icon><bitmap>0g</bitmap></icon></chrp-boot>", {}, 0, &ok);  // bad hex
    CHECK(!ok);
    parse("<chrp-boot><foo>never closed</chrp-boot>", {}, 0, &ok);       // unknown, no end tag
    CHECK(!ok);
    const std::string big = "<chrp-boot><boot-script>" + std::string(BootInfo::kMaxScript, 'x') +
                            "</boot-script></chrp-boot>";
    parse(big, {}, 0, &ok);                                              // too long for the buffer
    CHECK(!ok);
    parse("<chrp-bootx></chrp-bootx>", {}, 0, &ok);                       // a different tag name
    CHECK(!ok);
    parse("", {}, 0, &ok);
    CHECK(!ok);
}

TEST_CASE("firmware/bootinfo/end_tag_prefix_and_attribute_spacing")
{
    bool ok = false;
    auto bi = parse("<chrp-boot><description>a</descriptions>b</description>"
                    "<icon size = 1 , 3><bitmap>aa bb cc</bitmap></icon></chrp-boot>", {}, 0, &ok);
    REQUIRE(ok);
    CHECK(std::string(bi->description) == "a</descriptions>b");  // not its end tag
    CHECK_EQ(bi->icon_width, 1u);
    CHECK_EQ(bi->icon_height, 3u);
    CHECK_EQ(bi->icon_pixels, 3u);
}
