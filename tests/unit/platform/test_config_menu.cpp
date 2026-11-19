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
};

} // namespace

TEST_CASE("platform/menu/main_page_and_memory")
{
    Rig r;
    CHECK(r.has("Memory"));
    CHECK(r.has("256 MB"));
    CHECK(r.has("CD-ROM: (empty)"));
    CHECK(r.has("no floppy drive"));
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
    r.press(kDown, 5);                                 // Memory, ROM, 4 drives -> ATA 1 slave
    CHECK(r.selected().find("ATA 1 slave") != std::string::npos);
    r.press(kDown);                                    // over the floppy line
    CHECK(r.selected().find("Start-up disk") != std::string::npos);
    r.press(kUp);
    CHECK(r.selected().find("ATA 1 slave") != std::string::npos);
    r.press(kEnd);
    CHECK(r.selected().find("Start the Mac") != std::string::npos);
    r.press(kHome);
    CHECK(r.selected().find("Memory") != std::string::npos);
    r.press(kUp);                                      // stays at the top
    CHECK(r.selected().find("Memory") != std::string::npos);
}

TEST_CASE("platform/menu/insert_a_cd_image")
{
    Rig r;
    r.press(kDown, 2);                                 // ATA 0 master (the CD-ROM)
    r.press(kEnter);
    CHECK(r.has("Drive: ATA 0 master"));
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
    CHECK(r.has("Drive: ATA 0 master"));               // back on the drive page
    r.press(kEscape);
    CHECK(r.has("CD-ROM: Mac OS 9.iso"));
}

TEST_CASE("platform/menu/folders_and_parent")
{
    Rig r;
    r.press(kDown, 2);
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
    CHECK(r.has("Drive: ATA 0 master"));
}

TEST_CASE("platform/menu/drive_type_readonly_and_eject")
{
    Rig r;
    r.press(kDown, 4);                                 // ATA 1 master: no drive
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
    r.press(kDown, 5);                                 // Start-up disk
    r.press(kRight);
    CHECK(r.menu.config().boot == MachineConfig::Boot::CdRom);
    r.press(kDown);                                    // Screen
    r.press(kLeft);
    CHECK_EQ(r.menu.config().width, 800u);
    CHECK(r.has("800 x 600"));
    r.press(kDown);                                    // Colours
    r.press(kRight);
    CHECK_EQ(r.menu.config().depth, 16u);
    CHECK(r.has("Thousands of colours"));
    r.press(kDown);
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
    r.press(kDown, 2);
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
    r.press(kDown, 4);                                 // ATA 1 master
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
    r.press(kDown, 2);
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
    menu.key(kDown); menu.key(kDown); menu.key(kDown); menu.key(kDown);  // ATA 1 master
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
    menu.key(kDown); menu.key(kDown);
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
