// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for platform/common/mac_session: building and starting the Mac from
// a configuration through a host interface, with a stand-in "Mac OS ROM"
// file (a CHRP boot script that moves a small ELF client to load-base and
// starts it, the mechanism Apple's file uses) and in-memory images.

#include "core/devices/ata/ata_bus.h"
#include "core/common/endian.h"
#include "core/machine/bw_firmware.h"
#include "core/storage/disk_format.h"
#include "core/machine/yosemite.h"
#include "platform/common/mac_session.h"
#include "platform/common/machine_state.h"
#include "tests/framework/g3test.h"
#include "tests/unit/storage/hfs_image.h"

#include <algorithm>
#include "tests/unit/machine/test_client.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace g3;
using g3::test_client::Client;

namespace {

constexpr u32 kRamMb = 64;

struct FakeHost final : MacHost {
    std::map<std::string, std::vector<u8>> files;
    std::vector<std::unique_ptr<RamBlockDevice>> open;
    u32 opened = 0, closed = 0;
    std::string console_text;
    std::vector<u32> ejected;
    const u8* load_file(const char* path, u32& size) override
    {
        auto it = files.find(path);
        if (it == files.end())
            return nullptr;
        size = static_cast<u32>(it->second.size());
        return it->second.data();
    }
    BlockDevice* open_image(const char* path, bool read_only) override
    {
        auto it = files.find(path);
        if (it == files.end())
            return nullptr;
        ++opened;
        open.push_back(std::make_unique<RamBlockDevice>(it->second.data(), it->second.size(), read_only));
        return open.back().get();
    }
    void close_image(BlockDevice*) override { ++closed; }
    void console(const char* t, u32 n) override { console_text.append(t, n); }
    void disc_ejected(u32 drive) override { ejected.push_back(drive); }
};

// The stand-in ROM file: bootinfo, then the client ELF at offset 0x1000.
std::vector<u8> boot_file(const Client& client)
{
    const std::string script =
        "<CHRP-BOOT><COMPATIBLE>MacRISC</COMPATIBLE><OS-NAME>Session test</OS-NAME>\n"
        "<BOOT-SCRIPT>\n"
        "\" booting\" type cr\n"
        "load-base 1000 + load-base 1000 move\n"
        "init-program go\n"
        "</BOOT-SCRIPT></CHRP-BOOT>\n";
    std::vector<u8> f(0x2000, 0);
    std::memcpy(f.data(), script.data(), script.size());
    const std::vector<u8> elf = client.elf();
    std::memcpy(&f[0x1000], elf.data(), elf.size());
    return f;
}

struct Rig {
    FakeHost host;
    Client client;
    MachineConfig cfg;
    std::vector<u8> ram = std::vector<u8>(static_cast<size_t>(kRamMb) << 20);
    std::vector<u8> vram = std::vector<u8>(8u << 20);
    MacSession session;
    Rig()
    {
        cfg.ram_mb = kRamMb;
        cfg.width = 640;
        cfg.height = 480;
        host.files[cfg.rom] = boot_file(client);
        cfg.drive[0].type = MachineConfig::DriveType::None;
    }
    MacSession::Memory memory()
    {
        MacSession::Memory m;
        m.ram = ram.data();
        m.ram_size = static_cast<u32>(ram.size());
        m.vram = vram.data();
        m.vram_size = static_cast<u32>(vram.size());
        m.blocks = blocks.data();
        m.blocks_size = static_cast<u32>(blocks.size());
        return m;
    }
    std::vector<u8> blocks = std::vector<u8>(2u << 20);
    MacSession::Error start() { return session.start(cfg, host, memory(), nullptr, 0, 1000000000ull); }
};

} // namespace

TEST_CASE("platform/session/boots_the_rom_file_and_runs")
{
    Rig r;
    REQUIRE(r.start() == MacSession::Error::None);
    CHECK(r.session.running());
    CHECK(r.session.firmware()->client_started());
    CHECK_EQ(r.session.firmware()->client_entry(), Client::kCode);
    r.session.run(1000000000ull);  // 1 ms of guest time
    CHECK(r.host.console_text == "booting\n" + r.client.msg);
    CHECK(!r.session.halted());
    // The configured display mode is on.
    CHECK_EQ(r.session.screen_width(), 640u);
    CHECK_EQ(r.session.screen_height(), 480u);
    std::vector<u32> px(640 * 480);
    r.session.screen(px.data(), 640);
    // The RTC is the host's time (seconds since 1904 in the Cuda).
    CHECK_EQ(r.session.machine()->macio().cuda().rtc(), static_cast<u32>(1000000000ull + 2082844800ull));
}

TEST_CASE("platform/session/keyboard_and_mouse_reach_adb")
{
    Rig r;
    REQUIRE(r.start() == MacSession::Error::None);
    AdbKeyboard& kb = r.session.machine()->keyboard();
    r.session.key(0x04, true);           // HID 'a'
    r.session.key(0x04, false);
    u8 reg0[8] = {};
    REQUIRE(kb.talk(0, reg0) == 2u);     // Talk Register 0: two key events
    CHECK_EQ(reg0[0], 0x00u);            // ADB 'a' down
    CHECK_EQ(reg0[1], 0x80u);            // and up
    r.session.key(0x01, true);           // a usage with no ADB key: ignored
    CHECK(!kb.has_data());
    r.session.mouse(5, -3, 1);
    CHECK(r.session.machine()->mouse().has_data());
}

TEST_CASE("platform/session/drives_from_the_config")
{
    Rig r;
    r.host.files["/disks/hd.img"] = std::vector<u8>(64 * 512, 0);
    r.host.files["/disks/a.iso"] = std::vector<u8>(10 * 2048, 0);
    r.host.files["/disks/b.iso"] = std::vector<u8>(20 * 2048, 0);
    r.cfg.drive[0].type = MachineConfig::DriveType::CdRom;
    REQUIRE(r.cfg.set_image(0, "/disks/a.iso"));
    r.cfg.drive[2].type = MachineConfig::DriveType::Disk;
    REQUIRE(r.cfg.set_image(2, "/disks/hd.img"));
    REQUIRE(r.start() == MacSession::Error::None);
    CHECK_EQ(r.host.opened, 2u);
    // ata0.master answers as ATAPI, ata1.master as a disk.
    AtaBus& b0 = r.session.machine()->macio().ide(0).bus();
    AtaBus& b1 = r.session.machine()->macio().ide(1).bus();
    b0.write_reg(ata::kDevice, 0);
    CHECK_EQ(b0.read_reg(ata::kLbaHigh), 0xEBu);
    CHECK((b1.read_alt_status() & ata::DRDY) != 0);
    // Swap the disc, then eject it; a disk is not a disc.
    CHECK(r.session.change_disc(0, "/disks/b.iso"));
    CHECK_EQ(r.host.closed, 1u);
    CHECK(!r.session.change_disc(0, "/disks/missing.iso"));
    CHECK(!r.session.change_disc(2, "/disks/b.iso"));
    CHECK(r.session.change_disc(0, ""));
    CHECK_EQ(r.host.closed, 2u);
    // Mac OS ejecting the disc (START STOP UNIT) closes it and tells the host.
    CHECK(r.session.change_disc(0, "/disks/a.iso"));
    b0.write_reg(ata::kFeatures, 0);
    b0.write_reg(ata::kCommand, 0xA0);
    const u8 eject[12] = {0x1B, 0, 0, 0, 0x02};
    for (u32 i = 0; i < 12; i += 2)
        b0.write_data(&eject[i], 2);
    CHECK(r.host.ejected == std::vector<u32>({0u}));
    CHECK_EQ(r.host.closed, 3u);
    // stop() closes what is still open (the hard disk).
    r.session.stop();
    CHECK_EQ(r.host.closed, 4u);
    CHECK(!r.session.running());
}

TEST_CASE("platform/session/start_failures")
{
    {
        Rig r;
        r.ram.resize(32u << 20);            // not the configured 64 MB
        CHECK(r.start() == MacSession::Error::BadRam);
    }
    {
        Rig r;
        r.host.files.clear();
        CHECK(r.start() == MacSession::Error::NoRom);
        CHECK(std::string(r.session.detail()) == r.cfg.rom);
        CHECK(!r.session.running());
    }
    {
        Rig r;
        r.cfg.drive[2].type = MachineConfig::DriveType::Disk;
        REQUIRE(r.cfg.set_image(2, "/disks/nothing.img"));
        CHECK(r.start() == MacSession::Error::Image);
        CHECK(std::string(r.session.detail()) == "/disks/nothing.img");
    }
    {
        Rig r;
        r.host.files[r.cfg.rom] = std::vector<u8>(4096, 0x41);   // not a Mac OS ROM file
        CHECK(r.start() == MacSession::Error::NotMacOsRom);
        CHECK(!r.session.running());
        CHECK(std::string(r.session.detail()).find("not \"Mac OS ROM\"") != std::string::npos);
    }
    {
        Rig r;
        r.host.files[r.cfg.rom] = std::vector<u8>(4u << 20, 0x00);  // an older Mac's ROM chip, dumped
        CHECK(r.start() == MacSession::Error::NotMacOsRom);
        CHECK(std::string(r.session.detail()).find("ROM chip image") != std::string::npos);
    }
    {
        Rig r;  // a CHRP boot file whose script fails
        const std::string bad = "<CHRP-BOOT><BOOT-SCRIPT>\nnot-a-word\n</BOOT-SCRIPT></CHRP-BOOT>\n";
        r.host.files[r.cfg.rom] = std::vector<u8>(bad.begin(), bad.end());
        CHECK(r.start() == MacSession::Error::BootFailed);
        CHECK(std::string(MacSession::error_text(MacSession::Error::BootFailed)).find("did not start") !=
              std::string::npos);
    }
}

TEST_CASE("platform/session/rom_file_from_the_cd")
{
    // No usable ROM file in the settings, but a CD image whose System
    // Folder holds "Mac OS ROM": the Mac starts from that.
    for (int with_chip = 0; with_chip < 2; ++with_chip) {
        Rig r;
        hfs_image::Volume vol;
        const std::vector<u8> rom = boot_file(r.client);
        auto it = std::search(vol.img.d.begin(), vol.img.d.end(), vol.rom.begin(), vol.rom.begin() + 64);
        REQUIRE(it != vol.img.d.end());
        std::copy(rom.begin(), rom.end(), it);
        r.host.files["/cd/Mac OS 9.iso"] = vol.img.d;
        r.cfg.drive[0].type = MachineConfig::DriveType::CdRom;
        REQUIRE(r.cfg.set_image(0, "/cd/Mac OS 9.iso"));
        if (with_chip)
            r.host.files[r.cfg.rom] = std::vector<u8>(4u << 20, 0x00);
        else
            r.host.files.erase(r.cfg.rom);
        REQUIRE(r.start() == MacSession::Error::None);
        CHECK(r.session.firmware()->client_started());
        CHECK(std::string(r.session.rom_source()).find("System Folder of the CD") != std::string::npos);
        r.session.run(1000000000ull);
        CHECK(r.host.console_text.find("booting") != std::string::npos);
    }
}

TEST_CASE("platform/session/jit_option_turns_on_the_block_cache")
{
    for (bool on : {false, true}) {
        Rig r;
        r.cfg.jit = on;
        REQUIRE(r.start() == MacSession::Error::None);
        CHECK_EQ(r.session.block_cache_on(), on);
        CHECK(!r.session.jit_on());  // no code memory given (and not an AArch64 host here)
        r.session.run(1000000000ull);
        CHECK(r.host.console_text == "booting\n" + r.client.msg);  // same behaviour either way
    }
}

TEST_CASE("platform/session/start_up_disk_sets_chosen_bootpath")
{
    // A CD (2048-byte-block map, as Apple CDs) on ata0 and a disk on ata1:
    // /chosen bootpath names the chosen drive, its HFS partition and \\:tbxi.
    // The disk is blank (no System Folder): automatic start-up takes the CD;
    // boot = disk takes the disk all the same.
    std::vector<u8> cd(65536 * 512, 0), hd(65536 * 512, 0);
    HfsFormatOptions opt;
    REQUIRE(format_apm_hfs(cd.data(), cd.size(), opt));
    REQUIRE(format_apm_hfs(hd.data(), hd.size(), opt));
    store_be16(&cd[2], 2048);
    for (int boot = 0; boot < 4; ++boot) {
        Rig r;
        r.host.files["/d/cd.iso"] = cd;
        r.host.files["/d/hd.img"] = hd;
        r.cfg.drive[0].type = MachineConfig::DriveType::CdRom;
        REQUIRE(r.cfg.set_image(0, "/d/cd.iso"));
        if (boot != 1) {  // 1: no disk
            r.cfg.drive[2].type = MachineConfig::DriveType::Disk;
            REQUIRE(r.cfg.set_image(2, "/d/hd.img"));
        }
        r.cfg.boot = boot == 2   ? MachineConfig::Boot::CdRom
                     : boot == 3 ? MachineConfig::Boot::Disk
                                 : MachineConfig::Boot::Auto;
        REQUIRE(r.start() == MacSession::Error::None);
        const of::DeviceTree& t = r.session.firmware()->tree();
        const of::BwNodes& n = r.session.firmware()->nodes();
        const u8* data = nullptr;
        u32 len = 0;
        REQUIRE(t.get_property(n.chosen, "bootpath", data, len));
        char want[256];
        const u32 k = t.path_of(boot == 3 ? n.cmd646_disk : n.disk[0], want, sizeof want);
        std::strcpy(want + k, ":2,\\\\:tbxi");
        CHECK(std::string(reinterpret_cast<const char*>(data)) == want);
    }
}

TEST_CASE("platform/session/state_survives_a_restart")
{
    // First run: change NVRAM (as Mac OS would, through the firmware's
    // nvram access) and PRAM, then capture.
    MachineState st;
    {
        Rig r;
        REQUIRE(r.start() == MacSession::Error::None);
        r.session.run(1000000000ull);
        r.session.machine()->macio().nvram().set_byte(0x11B0, 0x5A);
        r.session.machine()->macio().cuda().pram()[0x13] = 0xA5;
        REQUIRE(r.session.capture_state(st));
        CHECK_EQ(st.nvram[0x11B0], 0x5Au);
        CHECK_EQ(st.pram[0x13], 0xA5u);
        CHECK(st.rtc_1904 >= static_cast<u32>(1000000000ull + kUnixTo1904));
    }
    // Through the file format, into a second run.
    std::vector<u8> file(MachineState::kFileSize);
    st.encode(file.data());
    MachineState back;
    REQUIRE(back.decode(file.data(), static_cast<u32>(file.size())));
    Rig r2;
    REQUIRE(r2.session.start(r2.cfg, r2.host, r2.memory(), nullptr, 0, 1000000000ull, &back) ==
            MacSession::Error::None);
    CHECK_EQ(r2.session.machine()->macio().nvram().byte(0x11B0), 0x5Au);
    CHECK_EQ(r2.session.machine()->macio().cuda().pram()[0x13], 0xA5u);
    // The firmware kept the saved (valid) NVRAM rather than formatting it:
    // the rest of it matches too.
    for (u32 i = 0; i < MachineState::kNvramSize; i += 97)
        CHECK_EQ(r2.session.machine()->macio().nvram().byte(i), st.nvram[i]);
    MachineState none;
    Rig r3;
    CHECK(!r3.session.capture_state(none));  // no Mac running
}

TEST_CASE("platform/session/set_clock_while_running")
{
    // Internet time arriving after the Mac started: its real-time clock
    // (Cuda, seconds since 1904) is set at once.
    Rig r;
    REQUIRE(r.start() == MacSession::Error::None);
    r.session.set_clock(1767225600u);  // 2026-01-01 00:00:00 as the Mac's local time
    CHECK_EQ(r.session.machine()->macio().cuda().rtc(), static_cast<u32>(1767225600u + 2082844800u));
}
