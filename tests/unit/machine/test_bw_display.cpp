// SPDX-License-Identifier: GPL-3.0-or-later
// The display in the B&W machine: device tree node (as Linux offb reads it),
// the Open Firmware display methods, and the pixels they produce.

#include "core/machine/bw_firmware.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kRam = 16u << 20, kVramSize = 4u << 20;

struct Rig {
    std::vector<u8> ram = std::vector<u8>(kRam);
    std::vector<u8> vram = std::vector<u8>(kVramSize, 0x77);
    std::unique_ptr<YosemiteMachine> m;
    std::unique_ptr<BwFirmware> fw;
    explicit Rig(bool with_display = true, u32 depth = 8)
    {
        YosemiteConfig cfg;
        cfg.ram = ram.data();
        cfg.ram_size = kRam;
        if (with_display) {
            cfg.vram = vram.data();
            cfg.vram_size = kVramSize;
        }
        m = std::make_unique<YosemiteMachine>(cfg);
        BwFirmware::Config fc;
        fc.depth = depth;
        fw = std::make_unique<BwFirmware>(*m, fc);
    }
    u32 prop(of::Phandle h, const char* name)
    {
        const u8* d = nullptr;
        u32 len = 0;
        if (!fw->tree().get_property(h, name, d, len) || len < 4)
            return 0xFFFFFFFFu;
        return (static_cast<u32>(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
    }
    std::vector<s32> stack(const std::string& s)
    {
        std::vector<s32> v;
        if (!fw->command(s.c_str()))
            return {0x7EADBEEF};
        while (fw->forth().depth())
            v.insert(v.begin(), static_cast<s32>(fw->forth().pop()));
        return v;
    }
};

using V = std::vector<s32>;

} // namespace

TEST_CASE("machine/bw-display/node_as_offb_reads_it")
{
    Rig r;
    REQUIRE(r.fw->ok());
    REQUIRE(r.m->has_display());
    const of::Phandle d = r.fw->nodes().display;
    REQUIRE(d != of::kNone);
    of::DeviceTree& t = r.fw->tree();
    const u8* p = nullptr;
    u32 len = 0;
    REQUIRE(t.get_property(d, "compatible", p, len));
    CHECK(std::string(reinterpret_cast<const char*>(p)) == "display");   // offb match table
    REQUIRE(t.get_property(d, "device_type", p, len));
    CHECK(std::string(reinterpret_cast<const char*>(p)) == "display");
    CHECK_EQ(r.prop(d, "width"), 640u);
    CHECK_EQ(r.prop(d, "height"), 480u);
    CHECK_EQ(r.prop(d, "depth"), 8u);
    CHECK_EQ(r.prop(d, "linebytes"), 640u);
    CHECK_EQ(r.prop(d, "address"), YosemiteMachine::kVramBase);
    // reg is a root-level (1:1) address range that covers the frame buffer.
    REQUIRE(t.get_property(d, "reg", p, len));
    // Then the register block (for our Mac OS display driver).
    REQUIRE(len == 16);
    auto cell = [&](u32 i) { return (static_cast<u32>(p[4 * i]) << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3]; };
    CHECK_EQ(cell(0), YosemiteMachine::kVramBase);
    CHECK_EQ(cell(1), kVramSize);
    CHECK_EQ(cell(2), YosemiteMachine::kDisplayRegs);
    CHECK_EQ(cell(3), Framebuffer::kRegsSize);
    CHECK_EQ(t.find("screen"), d);
    // Power-on picture: enabled, black.
    CHECK(r.m->display().enabled());
    CHECK_EQ(r.vram[0], 0u);
    CHECK_EQ(r.vram[640 * 480 - 1], 0u);
    CHECK_EQ(r.vram[640 * 480], 0x77u);                       // beyond the picture: untouched
}

TEST_CASE("machine/bw-display/forth_display_methods")
{
    Rig r;
    REQUIRE(r.fw->ok());
    CHECK(r.stack("\" screen\" open-dev dup value scr 0<>") == V({-1}));
    // color! ( r g b index -- ) and color@.
    CHECK(r.stack("decimal 255 128 0 5 \" color!\" scr $call-method") == V({}));
    CHECK_EQ(r.m->display().palette(5), 0xFF8000u);
    CHECK(r.stack("5 \" color@\" scr $call-method") == V({255, 128, 0}));
    // fill-rectangle ( index x y w h -- ), clipped.
    CHECK(r.stack("5 10 20 3 2 \" fill-rectangle\" scr $call-method") == V({}));
    CHECK_EQ(r.vram[20 * 640 + 10], 5u);
    CHECK_EQ(r.vram[21 * 640 + 12], 5u);
    CHECK_EQ(r.vram[21 * 640 + 13], 0u);
    CHECK_EQ(r.vram[22 * 640 + 10], 0u);
    CHECK(r.stack("7 630 470 100 100 \" fill-rectangle\" scr $call-method") == V({}));
    CHECK_EQ(r.vram[479 * 640 + 639], 7u);
    CHECK_EQ(r.vram[640 * 480], 0x77u);                       // nothing written past the screen
    CHECK(r.stack("\" dimensions\" scr $call-method") == V({640, 480}));
    // set-colors ( adr index #indices -- ).
    r.ram[0x8000] = 1; r.ram[0x8001] = 2; r.ram[0x8002] = 3;
    r.ram[0x8003] = 4; r.ram[0x8004] = 5; r.ram[0x8005] = 6;
    CHECK(r.stack("hex 8000 10 2 \" set-colors\" scr $call-method") == V({}));
    CHECK_EQ(r.m->display().palette(0x10), 0x010203u);
    CHECK_EQ(r.m->display().palette(0x11), 0x040506u);
    // The converted picture shows the palette colour.
    std::vector<u32> out(640 * 480);
    r.m->display().convert(out.data(), 640);
    CHECK_EQ(out[20 * 640 + 10], 0xFF8000u);
    CHECK(r.stack("\" nosuch\" scr $call-method") == V({0x7EADBEEF}));
}

TEST_CASE("machine/bw-display/client_call_method_like_linux")
{
    // Linux prom_init: call-method("color!", ih, i, b, g, r).
    Rig r;
    REQUIRE(r.fw->ok());
    auto w = [&](u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) r.ram[a + k] = static_cast<u8>(v >> (24 - 8 * k)); };
    auto rd = [&](u32 a) { u32 v = 0; for (u32 k = 0; k < 4; ++k) v = (v << 8) | r.ram[a + k]; return v; };
    std::memcpy(&r.ram[0x9000], "open", 5);
    std::memcpy(&r.ram[0x9010], "screen", 7);
    std::memcpy(&r.ram[0x9020], "call-method", 12);
    std::memcpy(&r.ram[0x9030], "color!", 7);
    w(0xA000, 0x9000); w(0xA004, 1); w(0xA008, 1); w(0xA00C, 0x9010);
    REQUIRE(r.fw->client().call(0xA000) == 0u);
    const u32 ih = rd(0xA010);
    REQUIRE(ih != 0);
    w(0xA100, 0x9020); w(0xA104, 6); w(0xA108, 1);
    w(0xA10C, 0x9030); w(0xA110, ih); w(0xA114, 9); w(0xA118, 0x30); w(0xA11C, 0x20); w(0xA120, 0x10);
    CHECK_EQ(r.fw->client().call(0xA100), 0u);
    CHECK_EQ(rd(0xA124), 0u);                                  // catch-result
    CHECK_EQ(r.m->display().palette(9), 0x102030u);            // r 0x10, g 0x20, b 0x30
}

TEST_CASE("machine/bw-display/other_depths_and_no_display")
{
    {
        Rig r(true, 32);
        REQUIRE(r.fw->ok());
        CHECK_EQ(r.prop(r.fw->nodes().display, "depth"), 32u);
        CHECK_EQ(r.prop(r.fw->nodes().display, "linebytes"), 2560u);
        CHECK(r.stack("\" screen\" open-dev value scr  decimal 10 20 30 3 \" color!\" scr $call-method "
                      "3 1 1 1 1 \" fill-rectangle\" scr $call-method") == V({}));
        const size_t at = 1 * 2560 + 4 * 1;
        CHECK_EQ(r.vram[at + 1], 10u);
        CHECK_EQ(r.vram[at + 2], 20u);
        CHECK_EQ(r.vram[at + 3], 30u);
    }
    {
        Rig r(true, 15);
        REQUIRE(r.fw->ok());
        CHECK(r.stack("\" screen\" open-dev value scr  hex ff 0 0 4 \" color!\" scr $call-method "
                      "4 0 0 1 1 \" fill-rectangle\" scr $call-method") == V({}));
        CHECK_EQ(r.vram[0], 0x7Cu);                            // red: 0x7C00
        CHECK_EQ(r.vram[1], 0x00u);
        // A mid-range colour: 0x80 0x40 0x08 -> r 16, g 8, b 1 -> 0x4101.
        CHECK(r.stack("80 40 8 6 \" color!\" scr $call-method 6 1 0 1 1 \" fill-rectangle\" scr $call-method") == V({}));
        CHECK_EQ(r.vram[2], 0x41u);
        CHECK_EQ(r.vram[3], 0x01u);
    }
    {
        Rig r(false);
        REQUIRE(r.fw->ok());
        CHECK(!r.m->has_display());
        CHECK_EQ(r.fw->nodes().display, of::kNone);
        CHECK_EQ(r.fw->tree().find("screen"), of::kNone);
    }
}

// --- text terminal -------------------------------------------------------------------------

namespace {

struct TermRig : Rig {
    std::string echo;
    TermRig() : Rig(true, 8)
    {
        fw->set_console([](void* c, const char* t, u32 n) { static_cast<std::string*>(c)->append(t, n); }, &echo);
        fw->command("\" screen\" open-dev value scr");
    }
    void write(const std::string& s)
    {
        std::memcpy(&ram[0x20000], s.data(), s.size());
        fw->command(("hex 20000 " + std::to_string(s.size()) + " decimal \" write\" scr $call-method drop").c_str());
    }
    // The 8x16 cell at (col,row) as 128 bytes of palette indices.
    std::vector<u8> cell(u32 col, u32 row) const
    {
        std::vector<u8> c;
        for (u32 y = 0; y < 16; ++y)
            for (u32 x = 0; x < 8; ++x)
                c.push_back(vram[(row * 16 + y) * 640 + col * 8 + x]);
        return c;
    }
};

} // namespace

TEST_CASE("machine/bw-display/terminal_text")
{
    TermRig r;
    REQUIRE(r.fw->ok());
    r.write("Hi");
    // 'H' row 0 of the font is .#...#.. : pixels 1 and 5 lit, on both screen rows 0 and 1.
    const auto h = r.cell(0, 0);
    for (u32 y = 0; y < 2; ++y)
        for (u32 x = 0; x < 8; ++x)
            CHECK_EQ(h[y * 8 + x], (x == 1 || x == 5) ? 255u : 0u);
    CHECK_EQ(h[14 * 8 + 1], 0u);                              // font row 7 (blank) at screen rows 14-15
    CHECK(r.cell(1, 0) != r.cell(2, 0));                      // 'i' drawn, the next cell still blank
    CHECK(r.echo == "Hi");                                    // echoed to the log
    // New line, carriage return, backspace, tab.
    TermRig ref;
    ref.write("B");
    r.write("\nA\rB");
    CHECK(r.cell(0, 1) == ref.cell(0, 0));                    // B overwrote A at row 1, column 0
    CHECK_EQ(r.fw->nodes().display != of::kNone, true);
    r.write("xy\b\bZ");
    TermRig refz;
    refz.write("Z");
    CHECK(r.cell(1, 1) == refz.cell(0, 0));                   // backspaced twice, then Z
    r.write("\t!");
    TermRig refx;
    refx.write("!");
    CHECK(r.cell(8, 1) == refx.cell(0, 0));                   // tab to column 8
    // Wrap at 80 columns.
    TermRig w;
    w.write(std::string(80, '-') + "W");
    TermRig refw;
    refw.write("W");
    CHECK(w.cell(0, 1) == refw.cell(0, 0));
    // Form feed clears the screen and homes the cursor.
    r.write("\f");
    CHECK(r.cell(0, 0) == std::vector<u8>(128, 0));
}

TEST_CASE("machine/bw-display/terminal_scrolls")
{
    TermRig r;
    REQUIRE(r.fw->ok());
    // 31 lines on a 30-row screen: line i starts with 'A' + i.
    std::string text;
    for (int i = 0; i < 31; ++i)
        text += std::string(1, static_cast<char>('A' + i)) + (i < 30 ? "\n" : "");
    r.write(text);
    TermRig b, last;
    b.write("B");
    last.write(std::string(1, static_cast<char>('A' + 30)));
    CHECK(r.cell(0, 0) == b.cell(0, 0));                      // the first line scrolled away
    CHECK(r.cell(0, 29) == last.cell(0, 0));                  // the newest line at the bottom
    CHECK_EQ(r.vram[640 * 480], 0x77u);                       // scrolling stays inside the picture
}

TEST_CASE("machine/bw-display/stdout_is_the_screen")
{
    Rig r;
    REQUIRE(r.fw->ok());
    std::string echo;
    r.fw->set_console([](void* c, const char* t, u32 n) { static_cast<std::string*>(c)->append(t, n); }, &echo);
    // /chosen stdout is an instance of the display node.
    const u32 ih = r.prop(r.fw->nodes().chosen, "stdout");
    CHECK_EQ(r.fw->instances().package(ih), r.fw->nodes().display);
    // A client writes to stdout via the client interface: pixels appear and the log gets the text.
    auto w = [&](u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) r.ram[a + k] = static_cast<u8>(v >> (24 - 8 * k)); };
    std::memcpy(&r.ram[0x9000], "write", 6);
    std::memcpy(&r.ram[0x9010], "OK", 2);
    w(0xA000, 0x9000); w(0xA004, 3); w(0xA008, 1); w(0xA00C, ih); w(0xA010, 0x9010); w(0xA014, 2);
    CHECK_EQ(r.fw->client().call(0xA000), 0u);
    CHECK(echo == "OK");
    bool lit = false;
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x)
            lit |= r.vram[y * 640 + x] == 255;
    CHECK(lit);
    // 32-bit mode: white is 0x00FFFFFF.
    Rig t(true, 32);
    REQUIRE(t.fw->ok());
    t.fw->command("\" screen\" open-dev value scr");
    std::memcpy(&t.ram[0x20000], "H", 1);
    t.fw->command("hex 20000 1 \" write\" scr $call-method drop");
    const size_t px = 1 * 4;                                  // pixel (1, 0): lit in 'H'
    CHECK_EQ(t.vram[px + 1], 0xFFu);
    CHECK_EQ(t.vram[px + 3], 0xFFu);
    CHECK_EQ(t.vram[0 + 1], 0u);                              // pixel (0, 0): background
}

TEST_CASE("machine/bw-display/forth_output_goes_to_stdout")
{
    TermRig r;
    REQUIRE(r.fw->ok());
    r.echo.clear();
    CHECK(r.fw->command(".\" Hi\" decimal 42 ."));
    CHECK(r.echo == "Hi42 ");
    TermRig ref;
    ref.write("H");
    CHECK(r.cell(0, 0) == ref.cell(0, 0));                    // on the screen too
}

TEST_CASE("machine/bw-display/native_driver_property")
{
    // The display node carries the Mac OS driver it is given, byte for byte,
    // in "driver,AAPL,MacOS,PowerPC"; without one there is no such property.
    std::vector<u8> ram(kRam), vram(kVramSize);
    std::vector<u8> pef(12000);
    for (size_t i = 0; i < pef.size(); ++i)
        pef[i] = static_cast<u8>(i * 31 + 7);
    YosemiteConfig cfg;
    cfg.ram = ram.data();
    cfg.ram_size = kRam;
    cfg.vram = vram.data();
    cfg.vram_size = kVramSize;
    YosemiteMachine m(cfg);
    BwFirmware::Config fc;
    fc.display_driver = pef.data();
    fc.display_driver_size = static_cast<u32>(pef.size());
    BwFirmware fw(m, fc);
    REQUIRE(fw.ok());
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(fw.tree().get_property(fw.nodes().display, "driver,AAPL,MacOS,PowerPC", d, len));
    CHECK(len == pef.size());
    CHECK(std::equal(pef.begin(), pef.end(), d));
    Rig plain;
    REQUIRE(plain.fw->ok());
    CHECK(!plain.fw->tree().has_property(plain.fw->nodes().display, "driver,AAPL,MacOS,PowerPC"));
}
