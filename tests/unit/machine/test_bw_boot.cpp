// SPDX-License-Identifier: GPL-3.0-or-later
// End to end: the Blue & White machine with our firmware boots from a disk
// image. The boot file's bootinfo script loads an ELF client program,
// which runs on the emulated CPU and prints through the client interface.

#include "core/machine/bw_firmware.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"
#include "tests/unit/storage/hfs_image.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace g3;

namespace {

constexpr u32 kRam = 16u << 20;

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

// The client program: code at 0x6000, data at 0x7000 (absolute addressing
// with ra = 0 reaches both). It calls finddevice("/chosen"),
// getprop(chosen, "stdout", buf, 4) and write(stdout, msg, len).
struct Client {
    static constexpr u32 kCode = 0x6000, kData = 0x7000;
    static constexpr u32 kArgs1 = kData + 0x000, kArgs2 = kData + 0x040, kArgs3 = kData + 0x080;
    static constexpr u32 kBuf = kData + 0x0C0, kStrings = kData + 0x100;
    std::vector<u8> data = std::vector<u8>(0x200);
    std::vector<u32> code;
    std::string msg = "Hello from a client program\n";

    void w(u32 addr, u32 v)
    {
        const u32 o = addr - kData;
        for (u32 k = 0; k < 4; ++k) data[o + k] = static_cast<u8>(v >> (24 - 8 * k));
    }
    u32 s(u32& at, const std::string& str)
    {
        const u32 a = at;
        std::memcpy(&data[a - kData], str.c_str(), str.size() + 1);
        at = (at + static_cast<u32>(str.size()) + 4) & ~3u;
        return a;
    }
    Client()
    {
        using namespace asm_ppc;
        u32 at = kStrings;
        const u32 finddevice = s(at, "finddevice"), chosen = s(at, "/chosen"), getprop = s(at, "getprop"),
                  stdout_name = s(at, "stdout"), write = s(at, "write"), m = s(at, msg);
        w(kArgs1, finddevice); w(kArgs1 + 4, 1); w(kArgs1 + 8, 1); w(kArgs1 + 12, chosen);
        w(kArgs2, getprop); w(kArgs2 + 4, 4); w(kArgs2 + 8, 1);
        w(kArgs2 + 16, stdout_name); w(kArgs2 + 20, kBuf); w(kArgs2 + 24, 4);
        w(kArgs3, write); w(kArgs3 + 4, 3); w(kArgs3 + 8, 1);
        w(kArgs3 + 16, m); w(kArgs3 + 20, static_cast<u32>(msg.size()));
        const u32 mr31_r5 = X(31, 5, 31, 5, 444);  // or r31,r5,r5 (mr r31,r5)
        code = {
            mr31_r5,
            stw(3, 0x7180, 0),                         // r3 on entry (must be 0)
            mtspr(ppc::spr::CTR, 31), li(3, static_cast<s16>(kArgs1)), bctr() | 1,
            lwz(4, kArgs1 + 16, 0), stw(4, kArgs2 + 12, 0),     // phandle of /chosen
            mtspr(ppc::spr::CTR, 31), li(3, static_cast<s16>(kArgs2)), bctr() | 1,
            lwz(4, kBuf, 0), stw(4, kArgs3 + 12, 0),            // the stdout ihandle
            mtspr(ppc::spr::CTR, 31), li(3, static_cast<s16>(kArgs3)), bctr() | 1,
            stw(3, 0x7184, 0),                         // status of the write call
            b(0),
        };
    }
    // An ELF file: header, two PT_LOAD segments (code, data).
    std::vector<u8> elf() const
    {
        std::vector<u8> e(0x600);
        auto b16 = [&](size_t a, u32 v) { e[a] = static_cast<u8>(v >> 8); e[a + 1] = static_cast<u8>(v); };
        auto b32 = [&](size_t a, u32 v) { b16(a, v >> 16); b16(a + 2, v & 0xFFFF); };
        e[0] = 0x7F; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 2; e[6] = 1;
        b16(16, 2); b16(18, 20); b32(20, 1); b32(24, kCode); b32(28, 52); b16(42, 32); b16(44, 2);
        const u32 code_bytes = static_cast<u32>(code.size() * 4);
        b32(52, 1); b32(56, 0x100); b32(60, kCode); b32(64, kCode); b32(68, code_bytes); b32(72, code_bytes);
        b32(84, 1); b32(88, 0x300); b32(92, kData); b32(96, kData); b32(100, 0x200); b32(104, 0x400);
        for (size_t i = 0; i < code.size(); ++i) b32(0x100 + 4 * i, code[i]);
        std::memcpy(&e[0x300], data.data(), data.size());
        return e;
    }
};

struct Machine {
    std::vector<u8> ram = std::vector<u8>(kRam);
    hfs_image::Volume vol;
    std::vector<u8> image;
    std::unique_ptr<RamBlockDevice> disk;
    std::unique_ptr<YosemiteMachine> m;
    std::unique_ptr<BwFirmware> fw;
    std::string console;
    explicit Machine(const std::string& boot_file, const std::vector<u8>& second_file)
    {
        // "Mac OS ROM" data: blocks 10-12; "Fragmented": first block 40.
        std::memcpy(&vol.img.d[10 * 4096], boot_file.data(), boot_file.size());
        std::memcpy(&vol.img.d[40 * 4096], second_file.data(), second_file.size());
        image = disk_with_volume(vol.img.d);
        disk = std::make_unique<RamBlockDevice>(image.data(), image.size());
        YosemiteConfig cfg;
        cfg.ram = ram.data();
        cfg.ram_size = kRam;
        m = std::make_unique<YosemiteMachine>(cfg);
        BwFirmware::Config fc;
        fc.disk[0] = disk.get();
        fw = std::make_unique<BwFirmware>(*m, fc);
        fw->set_console([](void* c, const char* t, u32 n) { static_cast<std::string*>(c)->append(t, n); },
                        &console);
    }
    u32 rd(u32 a) const { u32 v = 0; for (u32 k = 0; k < 4; ++k) v = (v << 8) | ram[a + k]; return v; }
};

const char* kBootInfo =
    "<CHRP-BOOT><COMPATIBLE>MacRISC</COMPATIBLE><OS-NAME>Test client</OS-NAME>\n"
    "<BOOT-SCRIPT>\n"
    "\" booting\" type cr\n"
    "load &device;:&partition;,\\System Folder\\Fragmented\n"
    "init-program go\n"
    "</BOOT-SCRIPT></CHRP-BOOT>\n";

} // namespace

TEST_CASE("machine/bw-boot/power_on_state")
{
    Machine mc(kBootInfo, Client().elf());
    REQUIRE(mc.fw->ok());
    BwFirmware& fw = *mc.fw;
    CHECK_EQ(mc.m->macio().mapped_base(), 0x80800000u);            // BAR assigned and enabled
    CHECK_EQ(fw.layout().base, kRam - BwFirmware::kFirmwareSize);
    CHECK(fw.memory().is_free(BwFirmware::kVectorsEnd, fw.layout().base - BwFirmware::kVectorsEnd));
    CHECK(!fw.memory().is_free(0, BwFirmware::kVectorsEnd));       // exception vectors stay out
    CHECK(!fw.memory().is_free(fw.layout().base, 1));
    CHECK_EQ(mc.rd(fw.layout().ci_entry), of::Firmware::kTrapClientInterface);
    // The tree knows the machine.
    const auto& n = fw.nodes();
    const u8* d = nullptr;
    u32 len = 0;
    REQUIRE(fw.tree().get_property(n.memory, "reg", d, len));
    CHECK_EQ(d[4], static_cast<u8>(kRam >> 24));
    REQUIRE(fw.tree().get_property(n.chosen, "stdout", d, len));
    CHECK(fw.instances().valid((static_cast<u32>(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3]));
    // Forth at the user interface, with the real tree.
    CHECK(fw.command("dev hd"));
    CHECK(fw.command("\" model\" \" /\" find-device active-package get-package-property drop 1- type cr"));
    // (output goes to the Forth output, not the console; checked through the stack instead:)
    CHECK(fw.command("\" /pci/pci-bridge/mac-io/via-cuda\" find-device active-package"));
    CHECK_EQ(fw.forth().pop(), n.cuda);
}

TEST_CASE("machine/bw-boot/boots_a_client_from_disk")
{
    Client client;
    Machine mc(kBootInfo, client.elf());
    REQUIRE(mc.fw->ok());
    REQUIRE(mc.fw->command("boot hd:2,\\\\:tbxi"));
    REQUIRE(mc.fw->client_started());
    CHECK_EQ(mc.fw->client_entry(), Client::kCode);
    CHECK_EQ(mc.m->cpu().state().pc, Client::kCode);
    // Virtual-Mode: the client starts with translation on (PowerPC binding §4.2).
    CHECK((mc.m->cpu().state().msr & (ppc::msr::IR | ppc::msr::DR)) == (ppc::msr::IR | ppc::msr::DR));
    // Run the machine: the client calls finddevice, getprop and write.
    mc.m->run_for(1000000000ull);                                 // 1 ms of guest time
    CHECK(mc.console == "booting\n" + client.msg);          // the script's output, then the client's
    CHECK_EQ(mc.rd(0x7180), 0u);                                  // r3 = 0 on entry
    CHECK_EQ(mc.rd(0x7184), 0u);                                  // write: service existed
    CHECK_EQ(mc.rd(Client::kArgs3 + 24), static_cast<u32>(client.msg.size()));  // bytes written
    CHECK(!mc.m->cpu().checkstopped());
    // The bss part of the data segment was zero-filled.
    CHECK_EQ(mc.rd(Client::kData + 0x200), 0u);
}

TEST_CASE("machine/bw-boot/failures")
{
    // Too little RAM.
    {
        std::vector<u8> ram(4u << 20);
        YosemiteConfig cfg;
        cfg.ram = ram.data();
        cfg.ram_size = static_cast<u32>(ram.size());
        YosemiteMachine m(cfg);
        BwFirmware fw(m, {});
        CHECK(!fw.ok());
        CHECK(!fw.command("1"));
    }
    // No disk: the hd alias exists but can't be opened.
    {
        std::vector<u8> ram(kRam);
        YosemiteConfig cfg;
        cfg.ram = ram.data();
        cfg.ram_size = kRam;
        YosemiteMachine m(cfg);
        BwFirmware fw(m, {});
        REQUIRE(fw.ok());
        CHECK(!fw.command("boot hd:2,\\\\:tbxi"));
        CHECK(!fw.client_started());
    }
}
