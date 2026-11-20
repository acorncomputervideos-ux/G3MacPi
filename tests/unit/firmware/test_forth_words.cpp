// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the Open Firmware Forth words, driven by Forth text as a boot
// script would use them.

#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/forth_words.h"
#include "core/mem/physical_bus.h"
#include "tests/framework/g3test.h"
#include "tests/unit/storage/hfs_image.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::of;

namespace {

constexpr u32 kRam = 0x100000;            // 1 MB of guest RAM
constexpr u32 kForthData = 0x80000, kForthSize = 0x10000;
constexpr u32 kScratch = 0x90000, kScratchSize = 0x4000;
constexpr u32 kLoadBase = 0x20000, kLoadLimit = 0x80000;

std::vector<u8> disk_with_volume(const std::vector<u8>& volume)
{
    auto be16 = [](std::vector<u8>& d, size_t at, u16 v) { d[at] = static_cast<u8>(v >> 8); d[at + 1] = static_cast<u8>(v); };
    auto be32 = [&](std::vector<u8>& d, size_t at, u32 v) { be16(d, at, static_cast<u16>(v >> 16)); be16(d, at + 2, static_cast<u16>(v)); };
    const u32 start = 64, blocks = static_cast<u32>((volume.size() + 511) / 512);
    std::vector<u8> d(static_cast<size_t>(start + blocks) * 512);
    be16(d, 0, 0x4552); be16(d, 2, 512); be32(d, 4, start + blocks);
    const char* types[] = {"Apple_partition_map", "Apple_HFS"};
    const u32 st[] = {1, start}, ct[] = {63, blocks};
    for (u32 i = 0; i < 2; ++i) {
        const size_t b = 512u * (1 + i);
        be16(d, b, 0x504D); be32(d, b + 4, 2); be32(d, b + 8, st[i]); be32(d, b + 12, ct[i]);
        std::memcpy(&d[b + 48], types[i], std::strlen(types[i]));
    }
    std::memcpy(&d[start * 512], volume.data(), volume.size());
    return d;
}

struct Rig {
    std::vector<u8> ram = std::vector<u8>(kRam);
    PhysicalBus bus;
    std::unique_ptr<DeviceTree> tree = std::make_unique<DeviceTree>();
    InstanceTable inst;
    hfs_image::Volume vol;
    std::vector<u8> image = disk_with_volume(vol.img.d);
    RamBlockDevice disk{image.data(), image.size()};
    std::unique_ptr<DiskDriver> drv = std::make_unique<DiskDriver>(disk);
    std::unique_ptr<forth::Forth> f;
    std::unique_ptr<FirmwareWords> words;
    std::string out;
    Phandle pci, macio, ata, hd, cpu;
    Rig()
    {
        bus.map_ram(0, ram.data(), kRam);
        DeviceTree& t = *tree;
        t.set_string(t.root(), "name", "device-tree");
        t.set_string(t.root(), "model", "PowerMac1,1");
        t.set_int(t.root(), "#address-cells", 1);
        Phandle cpus = t.add_node(t.root(), "cpus");
        cpu = t.add_node(cpus, "PowerPC,750", "0");
        t.set_int(cpu, "clock-frequency", 350000000);
        pci = t.add_node(t.root(), "pci", "80000000");
        macio = t.add_node(pci, "mac-io", "10");
        ata = t.add_node(macio, "ata", "20000");
        hd = t.add_node(ata, "disk", "0");
        const Phandle pk = t.add_node(t.root(), "packages");
        t.add_node(pk, "deblocker");
        const Phandle al = t.add_node(t.root(), "aliases");
        t.set_string(al, "hd", "/pci@80000000/mac-io@10/ata@20000/disk@0");
        inst.bind(hd, drv.get());
        f = std::make_unique<forth::Forth>(bus, kForthData, kForthSize);
        f->set_output([](void* c, const char* s, u32 n) { static_cast<std::string*>(c)->append(s, n); }, &out);
        words = std::make_unique<FirmwareWords>(*f, t, inst, kScratch, kScratchSize, kLoadBase, kLoadLimit);
    }
    bool run(const std::string& s) { return f->interpret(s.c_str(), static_cast<u32>(s.size())); }
    std::vector<s32> stack(const std::string& s)
    {
        std::vector<s32> v;
        if (!run(s))
            return {0x7EADBEEF};
        while (f->depth())
            v.insert(v.begin(), static_cast<s32>(f->pop()));
        return v;
    }
    std::string output(const std::string& s)
    {
        out.clear();
        if (!run(s))
            out += "<error " + std::to_string(f->error_code()) + ": " + f->error_text() + ">";
        return out;
    }
};

using V = std::vector<s32>;

} // namespace

TEST_CASE("forth-words/device_tree_queries")
{
    Rig r;
    CHECK(r.output("\" model\" \" /\" find-device active-package get-package-property drop 1- type") ==
          "PowerMac1,1");                                     // strings include their NUL
    // Missing property: just true.
    CHECK(r.stack("\" nothing\" active-package get-package-property") == V({-1}));
    CHECK(r.stack("dev /cpus/PowerPC,750 \" clock-frequency\" active-package "
                  "get-package-property drop decode-int nip nip") == V({350000000}));
    CHECK(r.stack("active-package") == V({static_cast<s32>(r.cpu)}));
    // Inherited: #address-cells is only on the root.
    CHECK(r.stack("\" #address-cells\" get-inherited-property drop decode-int nip nip") == V({1}));
    CHECK(r.stack("device-end active-package") == V({0}));
    CHECK(r.stack("\" deblocker\" find-package nip") == V({-1}));
    CHECK(r.stack("\" nope\" find-package") == V({0}));
    CHECK(r.stack("\" /pci\" find-package drop") == V({static_cast<s32>(r.pci)}));
    // Aliases work in dev.
    CHECK(r.stack("dev hd active-package") == V({static_cast<s32>(r.hd)}));
    CHECK(!r.run("dev /nowhere"));
    CHECK(!r.run("\" /nowhere\" find-device"));
}

TEST_CASE("forth-words/building_the_tree")
{
    Rig r;
    std::memset(&r.ram[kScratch], 0xFF, kScratchSize);       // no accidental NULs
    // A node built the way firmware Forth code builds one.
    CHECK(r.run("dev /pci/mac-io new-device \" via-cuda\" device-name \" via-cuda\" device-type "
                "h# 16000 encode-int h# 2000 encode-int encode+ \" reg\" property "
                "\" cuda\" encode-string \" compatible\" property finish-device"));
    const Phandle cuda = r.tree->find("/pci/mac-io/via-cuda");
    REQUIRE(cuda != kNone);
    CHECK_EQ(r.tree->parent(cuda), r.macio);
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(r.tree->get_property(cuda, "reg", d, len));
    REQUIRE(len == 8);
    CHECK_EQ(d[1], 0x01u);                                   // 0x00016000 big-endian
    CHECK_EQ(d[2], 0x60u);
    CHECK_EQ(d[6], 0x20u);                                   // 0x00002000
    REQUIRE(r.tree->get_property(cuda, "compatible", d, len));
    CHECK_EQ(len, 5u);                                       // "cuda" + NUL
    CHECK_EQ(d[4], 0u);
    REQUIRE(r.tree->get_property(cuda, "device_type", d, len));
    CHECK(std::strcmp(reinterpret_cast<const char*>(d), "via-cuda") == 0);
    CHECK(std::strcmp(r.tree->name(cuda), "via-cuda") == 0);
    CHECK_EQ(r.words->active_package(), r.macio);             // finish-device returned to the parent
    // Decoding: strings and ints from one property.
    CHECK(r.output("\" a\" encode-string \" bc\" encode-string encode+ 7 encode-int encode+ "
                   "decode-string type decode-string type decode-int . 2drop") == "abc7 ");
    CHECK(!r.run("device-end finish-device"));
    CHECK(!r.run("device-end 1 encode-int \" x\" property"));  // no active package
    CHECK(!r.run("3 0 decode-int"));                          // too short
}

TEST_CASE("forth-words/open_read_seek_and_load")
{
    Rig r;
    // Raw partition 2: the HFS Plus signature at byte 1024 of the volume.
    CHECK(r.stack("\" hd:2\" open-dev dup value ih 0<>") == V({-1}));
    CHECK(r.stack("400 0 \" seek\" ih $call-method") == V({0}));     // hex: 0x400
    CHECK(r.stack("1000 2 \" read\" ih $call-method 1000 c@ 1001 c@") == V({2, 0x48, 0x2B}));
    CHECK(r.run("ih close-dev"));
    CHECK(!r.run("1000 2 \" read\" ih $call-method"));               // closed
    CHECK(r.stack("\" hd:7\" open-dev") == V({0}));                   // no such partition
    // Reading more than the file holds returns what's there (10000 bytes).
    CHECK(r.stack("\" hd:2,\\\\:tbxi\" open-dev to ih  30000 8000 \" read\" ih $call-method") ==
          V({static_cast<s32>(hfs_image::Volume::kRomSize)}));
    CHECK(r.run("ih close-dev"));
    CHECK(!r.run("\" hd:2\" open-dev \" frob\" rot $call-method"));  // unsupported method
    // load: the boot file of the blessed folder, at load-base.
    const u32 open_before = r.inst.open_count();
    CHECK(r.run("load hd:2,\\\\:tbxi"));
    CHECK_EQ(r.words->load_size(), static_cast<u32>(hfs_image::Volume::kRomSize));
    CHECK(std::memcmp(&r.ram[kLoadBase], r.vol.rom.data(), r.vol.rom.size()) == 0);
    CHECK(r.stack("load-size load-base") == V({static_cast<s32>(hfs_image::Volume::kRomSize), static_cast<s32>(kLoadBase)}));
    CHECK_EQ(r.inst.open_count(), open_before);                       // load closes its instance
    // A path with a space (HFS names) reaches the end of the line.
    CHECK(r.run("load hd:2,\\System Folder\\Mac OS ROM\n"));
    CHECK_EQ(r.words->load_size(), static_cast<u32>(hfs_image::Volume::kRomSize));
    CHECK(!r.run("load hd:2,\\\\:zzzz"));
    CHECK(!r.run("load"));
}

// --- boot: bootinfo scripts and ELF programs ---------------------------------------------

namespace {

// Where file data of the test volume sits inside Rig::image.
constexpr size_t kVolume = 64 * 512;
size_t rom_at() { return kVolume + 10 * 4096; }     // "Mac OS ROM": blocks 10-12
size_t frag_at() { return kVolume + 40 * 4096; }    // "Fragmented": first block is 40

std::vector<u8> tiny_elf(u32 entry, u32 dest)
{
    std::vector<u8> e(0x200);
    auto b16 = [&](size_t a, u32 v) { e[a] = static_cast<u8>(v >> 8); e[a + 1] = static_cast<u8>(v); };
    auto b32 = [&](size_t a, u32 v) { b16(a, v >> 16); b16(a + 2, v & 0xFFFF); };
    e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 2; e[6] = 1;
    b16(16, 2); b16(18, 20); b32(20, 1); b32(24, entry); b32(28, 52); b16(42, 32); b16(44, 1);
    b32(52 + 0, 1); b32(52 + 4, 0x100); b32(52 + 8, dest); b32(52 + 12, dest);
    b32(52 + 16, 0x40); b32(52 + 20, 0x40);
    for (u32 i = 0; i < 0x40; ++i) e[0x100 + i] = static_cast<u8>(0xC0 + i);
    return e;
}

struct BootRig : Rig {
    u32 started = 0;
    int go_calls = 0;
    BootRig()
    {
        words->set_go([](void* c, u32 entry) {
            auto* b = static_cast<BootRig*>(c);
            b->started = entry;
            ++b->go_calls;
        }, this);
    }
    void put(size_t at, const std::string& s) { std::memcpy(&image[at], s.data(), s.size()); }
    void put(size_t at, const std::vector<u8>& v) { std::memcpy(&image[at], v.data(), v.size()); }
};

} // namespace

TEST_CASE("forth-words/boot_runs_a_bootinfo_script")
{
    BootRig r;
    r.put(rom_at(), std::string(
        "<CHRP-BOOT><COMPATIBLE>MacRISC</COMPATIBLE><OS-NAME>Test OS</OS-NAME>\n"
        "<BOOT-SCRIPT>\n"
        ".\" on &device; part &partition; [&directory;] &filename;\" cr\n"
        "decimal 40 2 +\n"
        "</BOOT-SCRIPT></CHRP-BOOT>"));
    CHECK(r.output("boot hd:2,\\\\:tbxi") == "on hd part 2 [] :tbxi\n");
    CHECK(r.stack("") == V({42}));                           // the script's results remain
    CHECK_EQ(r.go_calls, 0);
    CHECK(std::string(r.words->load_path()) == "hd:2,\\\\:tbxi");
    // The same file by its full path: directory and filename entities.
    CHECK(r.output("boot hd:2,\\System Folder\\Mac OS ROM") ==
          "on hd part 2 [System Folder] Mac OS ROM\n");
    r.stack("");
}

TEST_CASE("forth-words/boot_starts_an_elf_program")
{
    BootRig r;
    r.put(rom_at(), tiny_elf(0x12000, 0x12000));
    CHECK(r.run("boot hd:2,\\\\:tbxi"));
    CHECK_EQ(r.go_calls, 1);
    CHECK_EQ(r.started, 0x12000u);
    CHECK_EQ(r.ram[0x12000], 0xC0u);                         // segment copied into place
    CHECK_EQ(r.ram[0x1203F], 0xFFu);
    // load + init-program + go, step by step.
    CHECK(r.run("load hd:2,\\\\:tbxi"));
    CHECK(!r.words->program_ready());
    CHECK(r.run("init-program"));
    CHECK(r.words->program_ready());
    CHECK(r.run("go"));
    CHECK_EQ(r.go_calls, 2);
}

TEST_CASE("forth-words/bootinfo_script_loads_and_starts_an_elf")
{
    // The Mac-like chain: the boot file's script loads another file and runs it.
    BootRig r;
    r.put(rom_at(), std::string(
        "<chrp-boot><boot-script>\n"
        "load &device;:&partition;,\\System Folder\\Fragmented\n"
        "init-program go\n"
        "</boot-script></chrp-boot>"));
    r.put(frag_at(), tiny_elf(0x15000, 0x15000));
    CHECK(r.run("boot hd:2,\\\\:tbxi"));
    CHECK_EQ(r.go_calls, 1);
    CHECK_EQ(r.started, 0x15000u);
    CHECK_EQ(r.ram[0x15000], 0xC0u);
}

TEST_CASE("forth-words/boot_errors")
{
    BootRig r;
    // Not a known format (the test volume's pattern data).
    CHECK(!r.run("boot hd:2,\\\\:tbxi"));
    CHECK(!r.run("go"));                                      // nothing prepared
    CHECK(!r.run("boot hd:2,\\\\:nope"));
    // A boot script may not boot another bootinfo object.
    r.put(rom_at(), std::string("<chrp-boot><boot-script>boot hd:2,\\\\:tbxi</boot-script></chrp-boot>"));
    CHECK(!r.run("boot hd:2,\\\\:tbxi"));
    CHECK_EQ(r.f->error_code(), -21);
    // Bootinfo without a script.
    r.put(rom_at(), std::string("<chrp-boot><os-name>x</os-name></chrp-boot>                     "));
    CHECK(!r.run("boot hd:2,\\\\:tbxi"));
    // An ELF without a go callback.
    Rig plain;
    std::memcpy(&plain.image[rom_at()], tiny_elf(0x12000, 0x12000).data(), 0x200);
    CHECK(!plain.run("boot hd:2,\\\\:tbxi"));
}

// --- client interface: interpret and call-method ------------------------------------------

#include "core/firmware/openfirm/client_interface.h"

namespace {

struct CiForthRig : Rig {
    ClientInterface ci{*tree, bus};
    u32 heap = 0xA0000;
    u32 status = 0;
    CiForthRig()
    {
        ci.set_forth(f.get());
        ci.set_instances(&inst);
    }
    u32 str(const std::string& s)
    {
        const u32 a = heap;
        std::memcpy(&ram[a], s.c_str(), s.size() + 1);
        heap = (heap + static_cast<u32>(s.size()) + 4) & ~3u;
        return a;
    }
    void w(u32 a, u32 v) { for (u32 k = 0; k < 4; ++k) ram[a + k] = static_cast<u8>(v >> (24 - 8 * k)); }
    u32 rd(u32 a) const { u32 v = 0; for (u32 k = 0; k < 4; ++k) v = (v << 8) | ram[a + k]; return v; }
    std::vector<u32> call(const char* service, const std::vector<u32>& args, u32 nret)
    {
        const u32 a = 0xB0000;
        w(a, str(service)); w(a + 4, static_cast<u32>(args.size())); w(a + 8, nret);
        for (size_t i = 0; i < args.size(); ++i) w(a + 12 + 4 * static_cast<u32>(i), args[i]);
        status = ci.call(a);
        std::vector<u32> r;
        for (u32 i = 0; i < nret; ++i) r.push_back(rd(a + 12 + 4 * static_cast<u32>(args.size() + i)));
        return r;
    }
};

using U = std::vector<u32>;

} // namespace

TEST_CASE("forth-words/client_interpret")
{
    CiForthRig r;
    CHECK(r.call("interpret", {r.str("+"), 3, 4}, 2) == U({0, 7}));
    // Argument order: arg1 ends up on top, so "-" computes arg2 - arg1.
    CHECK(r.call("interpret", {r.str("-"), 10, 3}, 2) == U({0, static_cast<u32>(-7)}));
    // Results: ret1 is the top of the stack.
    CHECK(r.call("interpret", {r.str("1 2 3")}, 4) == U({0, 3, 2, 1}));
    // Missing results read as 0; extra stack items are discarded.
    CHECK(r.call("interpret", {r.str("5")}, 3) == U({0, 5, 0}));
    CHECK(r.call("interpret", {r.str("6 7 8")}, 2) == U({0, 8}));
    CHECK_EQ(r.f->depth(), 0u);
    // An error gives its throw code as catch-result.
    CHECK(r.call("interpret", {r.str("nonsense")}, 2) == U({static_cast<u32>(forth::Forth::kUndefined), 0}));
    CHECK_EQ(r.status, 0u);                                   // the service itself exists
    CHECK(r.call("test", {r.str("interpret")}, 1) == U({0}));
    CHECK(r.call("test", {r.str("call-method")}, 1) == U({0}));
}

TEST_CASE("forth-words/client_interpret_linux_style_node_building")
{
    // Adapted from Linux prom_init's fixups for firmware without some nodes.
    CiForthRig r;
    CHECK(r.call("interpret", {r.str(
        " s\" /pci\" find-device"
        " new-device"
            " 1 encode-int s\" #address-cells\" property"
            " 0 encode-int s\" #size-cells\" property"
            " s\" mdio\" device-name"
            " s\" fsl,mpc5200b-mdio\" encode-string s\" compatible\" property"
            " f0003000 400 reg"
            " 2 encode-int 5 encode-int encode+ 3 encode-int encode+ s\" interrupts\" property"
        " finish-device")}, 1) == U({0}));
    const Phandle mdio = r.tree->find("/pci/mdio");
    REQUIRE(mdio != kNone);
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(r.tree->get_property(mdio, "reg", d, len));
    CHECK_EQ(len, 8u);
    CHECK_EQ(d[0], 0xF0u);
    CHECK_EQ(d[6], 0x04u);
    REQUIRE(r.tree->get_property(mdio, "interrupts", d, len));
    CHECK_EQ(len, 12u);
    CHECK_EQ(d[7], 5u);
    // my-self ihandle>phandle inside new-device gives the new node.
    CHECK(r.call("interpret", {r.str(
        " s\" /pci/mdio\" find-device"
        " new-device"
            " s\" ethernet-phy\" device-name"
            " 10 encode-int s\" reg\" property"
            " my-self ihandle>phandle"
        " finish-device"
        " s\" /pci\" find-device encode-int s\" phy-handle\" property"
        " device-end")}, 1) == U({0}));
    const Phandle phy = r.tree->find("/pci/mdio/ethernet-phy");
    REQUIRE(phy != kNone);
    REQUIRE(r.tree->get_property(r.pci, "phy-handle", d, len));
    REQUIRE(len == 4);
    CHECK_EQ((static_cast<u32>(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3], phy);
    CHECK(r.call("interpret", {r.str("my-self")}, 2) == U({0, 0}));   // outside new-device
    CHECK(r.call("interpret", {r.str("1234 ihandle>phandle")}, 1) == U({static_cast<u32>(-21)}));
}

TEST_CASE("forth-words/client_call_method")
{
    CiForthRig r;
    const u32 ih = r.call("open", {r.str("hd:2")}, 1)[0];
    REQUIRE(ih != 0);
    // seek ( pos.lo pos.hi -- status ): arg1 = pos.hi (top), arg2 = pos.lo.
    CHECK(r.call("call-method", {r.str("seek"), ih, 0, 0x400}, 2) == U({0, 0}));
    // read ( addr len -- actual ): arg1 = len (top), arg2 = addr.
    CHECK(r.call("call-method", {r.str("read"), ih, 2, 0xC0000}, 2) == U({0, 2}));
    CHECK_EQ(r.ram[0xC0000], 0x48u);                          // 'H+'
    CHECK_EQ(r.ram[0xC0001], 0x2Bu);
    // Unknown method, bad ihandle: catch-result non-zero.
    CHECK(r.call("call-method", {r.str("frob"), ih}, 1)[0] != 0u);
    CHECK(r.call("call-method", {r.str("read"), 0x1234, 2, 0xC0000}, 2)[0] != 0u);
    CHECK_EQ(r.f->depth(), 0u);
    // Without a Forth interpreter the services don't exist.
    ClientInterface bare(*r.tree, r.bus);
    const u32 a = 0xB0000;
    r.w(a, r.str("interpret")); r.w(a + 4, 1); r.w(a + 8, 1); r.w(a + 12, r.str("1"));
    CHECK_EQ(bare.call(a), ClientInterface::kFail);
}
