// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for platform/common/config_menu: the settings menu, driven by key
// presses against a fake SD-card directory tree.

#include "platform/common/config_menu.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace g3;
using namespace g3::menu_key;

namespace {

struct FakeFs final : DirectoryLister {
    struct F { std::string name; bool dir; u64 size; };
    std::map<std::string, std::vector<F>> dirs;
    s32 list(const char* path, DirEntry* out, u32 max) override
    {
        auto it = dirs.find(path);
        if (it == dirs.end())
            return -1;
        u32 n = 0;
        for (const F& f : it->second) {
            if (n == max)
                break;
            std::strncpy(out[n].name, f.name.c_str(), DirEntry::kNameMax - 1);
            out[n].directory = f.dir;
            out[n].size = f.size;
            ++n;
        }
        return static_cast<s32>(n);
    }
};

FakeFs sample_fs()
{
    FakeFs fs;
    fs.dirs["/"] = {{"g3macpi", true, 0}};
    fs.dirs["/g3macpi"] = {{"disks", true, 0}, {"rom", true, 0}, {"g3macpi.ini", false, 300}};
    fs.dirs["/g3macpi/disks"] = {{"zeta.img", false, 100u << 20}, {"Installers", true, 0},
                                 {"Mac OS 9.iso", false, 650u << 20}, {"alpha.img", false, 2048}};
    fs.dirs["/g3macpi/disks/Installers"] = {{"Tools.iso", false, 50u << 20}};
    fs.dirs["/g3macpi/rom"] = {{"Mac OS ROM", false, 1945746}};
    return fs;
}

struct Rig {
    FakeFs fs = sample_fs();
    MachineConfig cfg;
    ConfigMenu menu{cfg, fs, "/g3macpi/disks", "/g3macpi/rom"};
    ConfigMenu::Screen scr;
    std::string screen()
    {
        menu.render(scr);
        std::string s;
        for (u32 r = 0; r < ConfigMenu::kRows; ++r) {
            s += scr.text[r];
            s += '\n';
        }
        return s;
    }
    // The text of the selected line.
    std::string selected()
    {
        menu.render(scr);
        for (u32 r = 0; r < ConfigMenu::kRows; ++r)
            if (scr.attr[r] == ConfigMenu::Attr::Selected)
                return scr.text[r];
        return "";
    }
    ConfigMenu::Action press(u8 k, int times = 1)
    {
        ConfigMenu::Action a = ConfigMenu::Action::None;
        for (int i = 0; i < times; ++i)
            a = menu.key(k);
        return a;
    }
    bool has(const std::string& text) { return screen().find(text) != std::string::npos; }
    // Moves the main page's selection to the line containing `text`.
    bool go(const std::string& text)
    {
        press(0x4A);  // Home
        for (int i = 0; i < 40; ++i) {
            if (selected().find(text) != std::string::npos)
                return true;
            press(0x51);  // Down
        }
        return false;
    }
};

} // namespace

TEST_CASE("platform/menu/main_page_and_memory")
{
    Rig r;
    CHECK(r.has("Memory"));
    CHECK(r.has("256 MB"));
    CHECK(r.has("CD-ROM: (empty)"));
    CHECK(!r.has("Floppy"));                          // the B&W has none: not listed
    CHECK(r.has("The Mac"));                          // grouped
    CHECK(r.has("Drives"));
    CHECK(r.has("Display"));
    CHECK(r.has("Network"));
    CHECK(r.has("The Pi"));
    CHECK(r.has("Start the Mac"));
    CHECK(r.selected().find("Memory") != std::string::npos);
    r.press(kRight);
    CHECK_EQ(r.menu.config().ram_mb, 384u);
    r.press(kRight);
    CHECK_EQ(r.menu.config().ram_mb, 512u);
    r.press(kRight);                                   // wraps to the smallest
    CHECK_EQ(r.menu.config().ram_mb, 64u);
    r.press(kLeft);                                    // and back
    CHECK_EQ(r.menu.config().ram_mb, 512u);
    CHECK(r.menu.dirty());
    CHECK(r.has("unsaved changes"));
}

TEST_CASE("platform/menu/navigation_skips_floppy")
{
    Rig r;
    r.press(kDown, 6);                                 // Memory, ROM, CPU, 4 drives -> Hard disk bus slave
    CHECK(r.selected().find("Hard disk bus slave") != std::string::npos);
    r.press(kDown);                                    // over the (hidden) floppy
    CHECK(r.selected().find("Start-up disk") != std::string::npos);
    r.press(kUp);
    CHECK(r.selected().find("Hard disk bus slave") != std::string::npos);
    r.press(kEnd);
    CHECK(r.selected().find("Restart the Pi") != std::string::npos);
    CHECK(r.press(kEnter) == ConfigMenu::Action::RestartPi);
    r.press(kUp);
    CHECK(r.selected().find("Start the Mac") != std::string::npos);
    r.press(kUp);
    CHECK(r.selected().find("Save settings") != std::string::npos);
    r.press(kUp);
    CHECK(r.selected().find("CPU self-test") != std::string::npos);
    CHECK(r.press(kEnter) == ConfigMenu::Action::SelfTest);
    r.press(kHome);
    CHECK(r.selected().find("Memory") != std::string::npos);
    r.press(kUp);                                      // stays at the top
    CHECK(r.selected().find("Memory") != std::string::npos);
}

TEST_CASE("platform/menu/insert_a_cd_image")
{
    Rig r;
    REQUIRE(r.go("CD bus master"));
    r.press(kEnter);
    CHECK(r.has("Drive: CD bus master"));
    CHECK(r.has("CD-ROM"));
    r.press(kDown);                                    // Disc image
    r.press(kEnter);                                   // file picker in /g3macpi/disks
    CHECK(r.has("Choose an image file"));
    const std::string s = r.screen();
    // Folders first, then files case-insensitively: Installers, alpha, Mac OS 9, zeta.
    const size_t up = s.find(".."), inst = s.find("Installers"), alpha = s.find("alpha.img"),
                 mac = s.find("Mac OS 9.iso"), zeta = s.find("zeta.img");
    CHECK(up < inst && inst < alpha && alpha < mac && mac < zeta);
    CHECK(s.find("650 MB") != std::string::npos);
    CHECK(s.find("2 KB") != std::string::npos);
    r.press(kDown, 3);                                 // .., Installers, alpha, Mac OS 9.iso
    CHECK(r.selected().find("Mac OS 9.iso") != std::string::npos);
    CHECK(r.press(kEnter) == ConfigMenu::Action::None);  // not running: no live media change
    CHECK(std::strcmp(r.menu.config().drive[0].image, "/g3macpi/disks/Mac OS 9.iso") == 0);
    CHECK(r.has("Drive: CD bus master"));               // back on the drive page
    r.press(kEscape);
    CHECK(r.has("CD-ROM: Mac OS 9.iso"));
}

TEST_CASE("platform/menu/folders_and_parent")
{
    Rig r;
    REQUIRE(r.go("CD bus master"));
    r.press(kEnter);
    r.press(kDown);
    r.press(kEnter);
    r.press(kDown);                                    // Installers
    r.press(kEnter);
    CHECK(r.has("/g3macpi/disks/Installers"));
    CHECK(r.has("Tools.iso"));
    r.press(kBackspace);                               // up one folder
    CHECK(r.has("zeta.img"));
    r.press(kEnter);                                   // ".." again (selection is on it)
    CHECK(r.has("g3macpi.ini"));
    r.press(kEscape);                                  // cancel: nothing chosen
    CHECK(r.menu.config().drive[0].image[0] == 0);
    CHECK(r.has("Drive: CD bus master"));
}

TEST_CASE("platform/menu/drive_type_readonly_and_eject")
{
    Rig r;
    REQUIRE(r.go("Hard disk bus master"));             // no drive
    r.press(kEnter);
    CHECK(r.has("No drive"));
    r.press(kDown);                                    // image/eject/read-only disabled: lands on Back
    CHECK(r.selected().find("Back") != std::string::npos);
    r.press(kUp);
    r.press(kRight);                                   // Type -> Hard disk
    CHECK(r.menu.config().drive[2].type == MachineConfig::DriveType::Disk);
    CHECK(!r.menu.config().drive[2].read_only);
    r.press(kDown);                                    // Disk image
    r.press(kEnter);
    r.press(kDown, 2);                                 // alpha.img
    r.press(kEnter);
    CHECK(std::strcmp(r.menu.config().drive[2].image, "/g3macpi/disks/alpha.img") == 0);
    r.press(kDown, 2);                                 // Remove image, Read-only
    CHECK(r.selected().find("Read-only") != std::string::npos);
    r.press(kEnter);
    CHECK(r.menu.config().drive[2].read_only);
    r.press(kUp);                                      // Remove image
    r.press(kEnter);
    CHECK(r.menu.config().drive[2].image[0] == 0);
    r.press(kUp, 2);
    r.press(kRight);                                   // Type -> CD-ROM: read-only, no image
    CHECK(r.menu.config().drive[2].type == MachineConfig::DriveType::CdRom);
    CHECK(r.menu.config().drive[2].read_only);
    r.press(kEscape);
    CHECK(r.has("CD-ROM: (empty)"));
}

TEST_CASE("platform/menu/rom_boot_display_and_actions")
{
    Rig r;
    r.press(kDown);                                    // ROM file
    r.press(kEnter);
    CHECK(r.has("Choose the Mac OS ROM file"));
    r.press(kDown);                                    // past ".."
    r.press(kEnter);
    CHECK(std::strcmp(r.menu.config().rom, "/g3macpi/rom/Mac OS ROM") == 0);
    CHECK(r.has("ROM file"));
    REQUIRE(r.go("Start-up disk"));
    r.press(kRight);
    CHECK(r.menu.config().boot == MachineConfig::Boot::CdRom);
    r.press(kDown);                                    // New hard disk...
    r.press(kDown);                                    // Resolution
    r.press(kLeft);
    CHECK_EQ(r.menu.config().width, 800u);
    CHECK(r.has("800 x 600"));
    r.press(kDown);                                    // Colours: millions by default
    r.press(kLeft);
    CHECK_EQ(r.menu.config().depth, 16u);
    CHECK(r.has("Thousands of colours"));
    r.press(kDown);                                    // Scaling: fill by default
    CHECK(r.has("Fill the screen"));
    r.press(kRight);
    CHECK(r.menu.config().sharp_pixels);
    CHECK(r.has("Sharp pixels"));
    r.press(kLeft);
    CHECK(!r.menu.config().sharp_pixels);
    r.press(kDown);                                    // Sound: on by default, Enter turns it off
    CHECK(r.has("On (HDMI)"));
    CHECK(r.press(kEnter) == ConfigMenu::Action::None);
    CHECK(!r.menu.config().sound);
    CHECK(r.menu.config().network);
    r.press(kDown);                                    // Ethernet
    r.press(kRight);
    CHECK(!r.menu.config().network);
    r.press(kLeft);
    CHECK(r.menu.config().network);
    CHECK(r.has("On (the Pi's Ethernet port)"));
    r.press(kDown);                                    // Network port: Ethernet or WiFi
    CHECK(r.has("Network port"));
    CHECK(!r.menu.config().wifi);
    r.press(kRight);
    CHECK(r.menu.config().wifi);
    CHECK(r.has("WiFi (no network: set wifi_name)"));
    CHECK(r.has("On (through the Pi's WiFi)"));
    CHECK(r.has("changes when the Pi restarts"));
    {
        MachineConfig named;                           // as if read from the file
        named.wifi = true;
        std::strcpy(named.wifi_name, "Home");
        ConfigMenu m2{named, r.fs, "/g3macpi/disks", "/g3macpi/rom"};
        ConfigMenu::Screen s2;
        m2.render(s2);
        bool found = false;
        for (const auto& row : s2.text)
            found = found || std::strstr(row, "WiFi: Home") != nullptr;
        CHECK(found);
    }
    r.press(kLeft);
    CHECK(!r.menu.config().wifi);
    CHECK(r.has("Ethernet"));
    r.press(kDown);                                    // Modem: off by default
    CHECK(!r.menu.config().modem);
    CHECK(r.press(kEnter) == ConfigMenu::Action::None);
    CHECK(r.menu.config().modem);
    CHECK(r.has("Internal 56K: dials up through the network"));
    r.press(kDown);                                    // At power-on
    CHECK(r.has("Start the Mac (F12: this menu)"));
    CHECK(r.press(kEnter) == ConfigMenu::Action::None);
    CHECK(!r.menu.config().autostart);
    CHECK(r.has("Show this menu"));
    r.press(kDown);                                    // Time zone: UTC by default, 30-minute steps
    CHECK(r.has("UTC+00:00"));
    r.press(kRight, 2);
    CHECK_EQ(r.menu.config().utc_offset_minutes, 60);
    CHECK(r.has("UTC+01:00"));
    r.press(kLeft, 4);
    CHECK_EQ(r.menu.config().utc_offset_minutes, -60);
    CHECK(r.has("UTC-01:00"));
    r.press(kDown);                                    // Pi CPU speed
    CHECK(r.has("1.8 GHz (standard)"));
    r.press(kRight);
    CHECK(r.menu.config().pi_overclock);
    CHECK(r.has("2.0 GHz overclock"));
    CHECK(r.has("2.0 GHz: written to config.txt by Save settings"));
    REQUIRE(r.go("CPU emulation"));                    // in The Mac group
    CHECK(r.has("Interpreter: safe, slower"));
    r.press(kRight);
    CHECK(r.menu.config().jit);
    CHECK(r.has("JIT: faster (new)"));
    REQUIRE(r.go("Save settings"));
    CHECK(r.press(kEnter) == ConfigMenu::Action::Save);
    r.menu.mark_saved();
    r.menu.set_status("Settings saved to /g3macpi/g3macpi.ini");
    CHECK(r.has("Settings saved"));
    CHECK(!r.menu.dirty());
    r.press(kDown);
    CHECK(r.press(kEnter) == ConfigMenu::Action::Start);
    CHECK(r.press(kEscape) == ConfigMenu::Action::Start);
}

TEST_CASE("platform/menu/running_mac_cd_swap_and_restart_marks")
{
    Rig r;
    r.menu.set_running(true);
    CHECK(r.has("Resume the Mac"));
    CHECK(!r.menu.needs_restart());
    // Insert a CD while running: takes effect at once.
    REQUIRE(r.go("CD bus master"));
    r.press(kEnter);
    r.press(kDown);
    r.press(kEnter);
    r.press(kDown, 3);
    CHECK(r.press(kEnter) == ConfigMenu::Action::MediaChanged);
    CHECK_EQ(r.menu.media_changed_drive(), 0u);
    CHECK(!r.menu.needs_restart());
    // Eject: also immediate.
    r.press(kDown);                                    // Eject disc
    CHECK(r.press(kEnter) == ConfigMenu::Action::MediaChanged);
    CHECK(r.menu.config().drive[0].image[0] == 0);
    r.press(kEscape);
    // Memory needs a restart: marked.
    r.press(kHome);
    r.press(kRight);
    CHECK(r.menu.needs_restart());
    CHECK(r.has("*Memory"));
    CHECK(r.has("takes effect when the Mac restarts"));
    CHECK(r.has("Restart the Mac"));
    CHECK(!r.has("Resume the Mac"));
    r.press(kLeft);
    CHECK(!r.menu.needs_restart());
    // A hard disk is never swapped live.
    REQUIRE(r.go("Hard disk bus master"));
    r.press(kEnter);
    r.press(kRight);                                   // -> Hard disk
    r.press(kDown);
    r.press(kEnter);
    r.press(kDown, 2);
    CHECK(r.press(kEnter) == ConfigMenu::Action::None);
    CHECK(r.menu.needs_restart());
}

TEST_CASE("platform/menu/unreadable_folder")
{
    Rig r;
    r.fs.dirs.erase("/g3macpi/disks");
    REQUIRE(r.go("CD bus master"));
    r.press(kEnter);
    r.press(kDown);
    r.press(kEnter);
    CHECK(r.has("cannot be read"));
    r.press(kEnter);                                   // ".." still works
    CHECK(r.has("g3macpi.ini"));
}

TEST_CASE("platform/menu/running_mac_hard_disk_image_needs_restart")
{
    // A hard disk present when the Mac started: changing its image or its
    // read-only flag waits for a restart (never a live swap).
    FakeFs fs = sample_fs();
    MachineConfig cfg;
    cfg.drive[2].type = MachineConfig::DriveType::Disk;
    REQUIRE(cfg.set_image(2, "/g3macpi/disks/zeta.img"));
    ConfigMenu menu(cfg, fs, "/g3macpi/disks", "/g3macpi/rom");
    menu.set_running(true);
    for (int k = 0; k < 5; ++k)
        menu.key(kDown);                               // Memory, ROM, CPU, 2 CD -> Hard disk bus master
    menu.key(kEnter);
    menu.key(kDown);                                   // Disk image
    menu.key(kEnter);
    // The picker opens with the current file (zeta.img) selected.
    menu.key(kHome);
    menu.key(kDown); menu.key(kDown);                  // .., Installers, alpha.img
    CHECK(menu.key(kEnter) == ConfigMenu::Action::None);
    CHECK(std::strcmp(menu.config().drive[2].image, "/g3macpi/disks/alpha.img") == 0);
    CHECK(menu.needs_restart());
    // Put it back: no restart needed; then the read-only flag alone.
    menu.key(kEnter);
    menu.key(kEnd);                                    // the list's last entry: zeta.img
    menu.key(kEnter);
    CHECK(std::strcmp(menu.config().drive[2].image, "/g3macpi/disks/zeta.img") == 0);
    CHECK(!menu.needs_restart());
    menu.key(kDown); menu.key(kDown);                  // Remove image, Read-only
    menu.key(kEnter);
    CHECK(menu.config().drive[2].read_only);
    CHECK(menu.needs_restart());
}

TEST_CASE("platform/menu/long_file_names_keep_the_size_column")
{
    FakeFs fs = sample_fs();
    fs.dirs["/g3macpi/disks"] = {{"Mac OS 9.2.2 Install.iso", false, 685u << 20},
                                 {std::string(70, 'x') + ".img", false, 1u << 20}};
    MachineConfig cfg;
    ConfigMenu menu(cfg, fs, "/g3macpi/disks", "/g3macpi/rom");
    for (int k = 0; k < 3; ++k)
        menu.key(kDown);                               // CD bus master
    menu.key(kEnter);
    menu.key(kDown);
    menu.key(kEnter);
    ConfigMenu::Screen scr;
    menu.render(scr);
    bool found_long = false, found_iso = false;
    for (u32 r = 0; r < ConfigMenu::kRows; ++r) {
        const std::string t = scr.text[r];
        if (t.find("Install.iso") != std::string::npos) {
            found_iso = true;
            CHECK(t.find("Install.iso ") != std::string::npos);   // a gap before the size
            CHECK_EQ(t.find("685 MB"), 56u);
        }
        if (t.find("xxx") != std::string::npos) {
            found_long = true;
            CHECK(t.find("...") != std::string::npos);
            CHECK_EQ(t.find("1 MB"), 56u);
        }
    }
    CHECK(found_iso);
    CHECK(found_long);
}

TEST_CASE("platform/menu/disc_ejected_by_the_mac")
{
    FakeFs fs = sample_fs();
    MachineConfig cfg;
    REQUIRE(cfg.set_image(0, "/g3macpi/disks/Mac OS 9.iso"));
    ConfigMenu menu(cfg, fs, "/g3macpi/disks", "/g3macpi/rom");
    menu.set_running(true);
    menu.disc_ejected(0);
    CHECK(menu.config().drive[0].image[0] == 0);
    CHECK(!menu.needs_restart());        // nothing waits for a restart
    CHECK(menu.dirty());                 // but the saved file still names the disc
    menu.disc_ejected(2);                // not a CD-ROM drive: no effect
    CHECK(menu.config().drive[2].type == MachineConfig::DriveType::None);
}

TEST_CASE("platform/menu/new_blank_hard_disk")
{
    Rig r;
    r.fs.dirs["/g3macpi/hdd"] = {{"HD1.IMG", false, 1u << 30}, {"notes.txt", false, 10}};
    r.fs.dirs["/g3macpi/cd"] = {{"Mac OS 9.iso", false, 650u << 20}};
    r.menu.set_media_folders("/g3macpi/cd", "/g3macpi/hdd");
    REQUIRE(r.go("Hard disk bus master"));             // no drive
    r.press(kEnter);
    // Only offered for hard disks.
    r.press(kRight);                                   // Type -> Hard disk
    CHECK(r.has("New blank disk"));
    CHECK(r.has("1 GB"));
    r.press(kDown, 3);                                 // Image, Read-only, New blank disk (Remove: no image)
    CHECK(r.selected().find("New blank disk") != std::string::npos);
    r.menu.mark_saved();
    r.press(kRight);                                   // 2 GB
    CHECK(r.has("2 GB"));
    CHECK_EQ(r.menu.new_disk_bytes(), 2048ull << 20);
    r.press(kRight, 2);                                // 4 GB (4095 MB: FAT32's limit), then wraps to 256 MB
    CHECK(r.has("256 MB"));
    CHECK_EQ(r.menu.new_disk_bytes(), 256ull << 20);
    r.press(kLeft);                                    // back to 4 GB
    CHECK_EQ(r.menu.new_disk_bytes(), 4095ull << 20);
    CHECK(!r.menu.dirty());                            // picking a size is not a settings change
    // Enter: a free name in the hard-disk folder (HD1.IMG exists, case-insensitively).
    CHECK(r.press(kEnter) == ConfigMenu::Action::CreateDisk);
    CHECK(std::string(r.menu.new_disk_path()) == "/g3macpi/hdd/hd2.img");
    // Failure: reported, the drive is unchanged.
    r.menu.disk_created(false, "Not enough space on the SD card.");
    CHECK(r.has("Not enough space on the SD card."));
    CHECK(r.menu.config().drive[2].image[0] == 0);
    // Success: the disk is in the drive, writable, and the settings changed.
    CHECK(r.press(kEnter) == ConfigMenu::Action::CreateDisk);
    r.menu.disk_created(true, nullptr);
    CHECK(std::strcmp(r.menu.config().drive[2].image, "/g3macpi/hdd/hd2.img") == 0);
    CHECK(r.menu.config().drive[2].type == MachineConfig::DriveType::Disk);
    CHECK(!r.menu.config().drive[2].read_only);
    CHECK(r.menu.dirty());                             // (the Pi then saves them at once)
    CHECK(r.has("Drive Setup"));
    // Back on the main page, on that drive's line, which shows the disk.
    CHECK(r.selected().find("Hard disk bus master") != std::string::npos);
    CHECK(r.selected().find("hd2.img") != std::string::npos);
    r.press(kEnter);                                   // its page again
    r.press(kDown, 3);                                 // Image, Remove, Read-only
    // A CD-ROM drive: the item can't be selected.
    r.press(kUp, 3);                                   // Read-only, Remove, Image, Type
    CHECK(r.selected().find("Type") != std::string::npos);
    r.press(kRight);                                   // CD-ROM
    r.press(kDown, 3);                                 // Disc image, Read-only? no: Eject disabled too -> Back
    CHECK(r.selected().find("New blank disk") == std::string::npos);
    r.press(kUp, 3);
    CHECK(r.selected().find("Type") != std::string::npos);
    r.press(kDown);                                    // Disc image: the picker starts in the CD folder
    r.press(kEnter);
    CHECK(r.has("/g3macpi/cd"));
    CHECK(r.has("Mac OS 9.iso"));
}

TEST_CASE("platform/menu/hard_disk_picker_starts_in_the_hdd_folder")
{
    Rig r;
    r.fs.dirs["/g3macpi/hdd"] = {{"system.img", false, 1u << 30}};
    r.menu.set_media_folders("/g3macpi/cd", "/g3macpi/hdd");
    REQUIRE(r.go("Hard disk bus master"));
    r.press(kEnter);
    r.press(kRight);                                   // Hard disk
    r.press(kDown);                                    // Disk image
    r.press(kEnter);
    CHECK(r.has("/g3macpi/hdd"));
    r.press(kDown);                                    // past ".."
    r.press(kEnter);                                   // system.img
    CHECK(std::strcmp(r.menu.config().drive[2].image, "/g3macpi/hdd/system.img") == 0);
    // No disks folder: creating a disk reports it.
    Rig q;
    q.menu.set_media_folders("/g3macpi/cd", "/nowhere");
    REQUIRE(q.go("Hard disk bus master"));
    q.press(kEnter);
    q.press(kRight);
    q.press(kDown, 3);
    CHECK(q.press(kEnter) == ConfigMenu::Action::None);
    CHECK(q.has("cannot be read"));
}

TEST_CASE("platform/menu/insert_media_from_outside")
{
    // The web interface puts discs into CD-ROM drives through the menu.
    Rig r;
    REQUIRE(r.menu.config().drive[0].type == MachineConfig::DriveType::CdRom);
    bool ok = false;
    // Mac stopped: the setting changes, nothing to tell the Mac.
    CHECK(r.menu.insert_media(0, "/g3macpi/cd/a.iso", ok) == ConfigMenu::Action::None);
    CHECK(ok);
    CHECK(std::strcmp(r.menu.config().drive[0].image, "/g3macpi/cd/a.iso") == 0);
    CHECK(r.menu.dirty());
    // Mac running: at once.
    r.menu.set_running(true);
    CHECK(r.menu.insert_media(0, "/g3macpi/cd/b.iso", ok) == ConfigMenu::Action::MediaChanged);
    CHECK(ok);
    CHECK_EQ(r.menu.media_changed_drive(), 0u);
    CHECK(!r.menu.needs_restart());
    CHECK(r.menu.insert_media(0, "", ok) == ConfigMenu::Action::MediaChanged);  // eject
    CHECK(r.menu.config().drive[0].image[0] == 0);
    // Not a CD-ROM drive: refused, unchanged.
    for (u32 d = 1; d < MachineConfig::kDrives; ++d)
        if (r.menu.config().drive[d].type != MachineConfig::DriveType::CdRom) {
            char before[MachineConfig::kPathMax];
            std::strcpy(before, r.menu.config().drive[d].image);
            CHECK(r.menu.insert_media(d, "/g3macpi/cd/a.iso", ok) == ConfigMenu::Action::None);
            CHECK(!ok);
            CHECK(std::strcmp(r.menu.config().drive[d].image, before) == 0);
        }
    CHECK(r.menu.insert_media(9, "/x", ok) == ConfigMenu::Action::None);
    CHECK(!ok);
}

TEST_CASE("platform/menu/long_status_wraps")
{
    Rig r;
    const std::string msg =
        "Cannot start: no Mac OS ROM file to start from: the ROM file is a ROM chip image of an older Mac. "
        "Use \"Mac OS ROM\" from Mac OS 9's System Folder, or put a Mac OS 9 CD image in the CD drive.";
    r.menu.set_status(msg.c_str());
    r.menu.render(r.scr);
    std::string shown;
    u32 lines = 0;
    for (u32 row = 0; row < ConfigMenu::kRows; ++row)
        if (r.scr.attr[row] == ConfigMenu::Attr::Status) {
            std::string t = r.scr.text[row];
            while (!t.empty() && t.back() == ' ')
                t.pop_back();
            while (!t.empty() && t.front() == ' ')
                t.erase(0, 1);
            shown += (shown.empty() ? "" : " ") + t;
            ++lines;
        }
    CHECK(lines >= 2);
    CHECK(lines <= 3);
    CHECK(shown == msg);  // every word, in order, none cut
    CHECK(r.has("Up/Down: select"));  // the help line is still there
}

TEST_CASE("platform/menu/wifi_page_typing")
{
    Rig r;
    // To "Network port", Enter opens the WiFi page.
    while (r.selected().find("Network port") == std::string::npos)
        r.press(kDown);
    r.press(kEnter);
    CHECK(r.has("WiFi network"));
    CHECK(r.selected().find("Use WiFi") != std::string::npos);
    r.press(kEnter);                                   // Use WiFi: yes
    CHECK(r.menu.config().wifi);
    CHECK(r.menu.dirty());
    r.press(kDown);                                    // Network name
    CHECK(r.has("(none) - press Enter to type it"));
    r.press(kEnter);
    CHECK(r.menu.typing());
    CHECK(r.has("Backspace: delete"));
    // "Home 5g", then Backspace and "G": Shift is the left-Shift bit.
    const u8 kH = 0x0B, kO = 0x12, kM = 0x10, kE = 0x08, k5 = 0x22, kG = 0x0A;
    r.menu.key(kH, 0x02);
    for (u8 k : {kO, kM, kE, static_cast<u8>(0x2C), k5, kG})
        r.menu.key(k);
    CHECK(r.has("Home 5g_"));
    r.press(kBackspace);
    r.menu.key(kG, 0x20);                              // right Shift
    CHECK(r.has("Home 5G_"));
    CHECK(r.press(kUp) == ConfigMenu::Action::None);   // arrows type nothing while typing
    r.press(kEnter);
    CHECK(!r.menu.typing());
    CHECK(std::strcmp(r.menu.config().wifi_name, "Home 5G") == 0);
    CHECK(r.has("Home 5G"));
    // Esc cancels typing.
    r.press(kEnter);
    r.menu.key(kH);
    r.press(kEscape);
    CHECK(std::strcmp(r.menu.config().wifi_name, "Home 5G") == 0);
    CHECK(r.has("WiFi network"));                      // still on the page
    // Password: too short is refused; shown as stars afterwards.
    r.press(kDown);
    r.press(kEnter);
    for (int i = 0; i < 5; ++i)
        r.menu.key(kE);
    r.press(kEnter);
    CHECK(r.menu.typing());
    CHECK(r.has("8 to 63 characters"));
    // The keyboard layout matters for symbols: US Shift+2 is @.
    r.menu.key(0x1F, 0x02);
    r.menu.key(0x34, 0x02);                            // US Shift+' is ", refused in a password
    CHECK(r.has("can't contain"));
    r.menu.key(kO);
    r.menu.key(kO);
    r.press(kEnter);
    CHECK(!r.menu.typing());
    CHECK(std::strcmp(r.menu.config().wifi_password, "eeeee@oo") == 0);
    CHECK(r.has("********"));
    CHECK(!r.has("eeeee@oo"));
    // Country: letters only, made capitals, two at most.
    r.press(kDown);
    r.press(kEnter);
    r.menu.key(kG);
    r.menu.key(0x1E);                                  // '1': ignored
    r.menu.key(0x05);                                  // 'b'
    r.menu.key(kE);                                    // a third letter: ignored
    r.press(kEnter);
    CHECK(std::strcmp(r.menu.config().wifi_country, "GB") == 0);
    // UK keyboard: Shift+2 types ", Shift+' types @.
    r.press(kDown);
    r.press(kRight);
    CHECK(r.menu.config().keyboard == MachineConfig::Keyboard::Uk);
    CHECK(r.has("UK"));
    r.press(kUp, 3);                                   // to Network name
    r.press(kEnter);
    r.menu.key(0x1F, 0x02);
    r.menu.key(0x34, 0x02);
    r.menu.key(0x2C);                                  // space, then '#' (UK: its own key): refused
    r.menu.key(0x32);
    CHECK(r.has("can't come first or after a space"));
    r.press(kBackspace);                               // the space
    r.press(kEnter);
    CHECK(std::strcmp(r.menu.config().wifi_name, "Home 5G\"@") == 0);
    // Back to the main page; the row shows the network.
    r.press(kEscape);
    CHECK(r.has("WiFi: Home 5G\"@"));
    // The settings file keeps it all.
    char out[4096];
    const u32 n = write_machine_config(r.menu.config(), out, sizeof out);
    MachineConfig back;
    ConfigParseReport rep;
    parse_machine_config(out, n, back, rep);
    CHECK_EQ(rep.ignored, 0u);
    CHECK(std::strcmp(back.wifi_name, "Home 5G\"@") == 0);
    CHECK(std::strcmp(back.wifi_password, "eeeee@oo") == 0);
    CHECK(back.keyboard == MachineConfig::Keyboard::Uk);
}

TEST_CASE("platform/menu/new_hard_disk_shortcut")
{
    // "New hard disk...": the first free position, hard disk bus master
    // first, becomes a hard disk; its page opens on "New blank disk".
    Rig r;
    REQUIRE(r.go("New hard disk"));
    CHECK(r.press(kEnter) == ConfigMenu::Action::None);
    CHECK(r.selected().find("New blank disk") != std::string::npos);
    CHECK(r.has("Hard disk bus master"));
    CHECK(r.menu.config().drive[2].type == MachineConfig::DriveType::Disk);
    CHECK(r.press(kEnter) == ConfigMenu::Action::CreateDisk);   // with the size shown
    // All positions taken by images: a hint instead.
    Rig full;
    for (auto& d : full.cfg.drive) {
        d.type = MachineConfig::DriveType::Disk;
        std::strcpy(d.image, "/g3macpi/hdd/x.img");
    }
    ConfigMenu m{full.cfg, full.fs, "/g3macpi/disks", "/g3macpi/rom"};
    for (int i = 0; i < 8; ++i)
        m.key(0x51);  // Down to New hard disk (past Memory, ROM, CPU, 4 drives, Start-up disk)
    m.key(0x28);      // Enter
    ConfigMenu::Screen sc;
    m.render(sc);
    bool hint = false;
    for (const auto& row : sc.text)
        hint = hint || std::strstr(row, "All four drive positions are in use") != nullptr;
    CHECK(hint);
}
