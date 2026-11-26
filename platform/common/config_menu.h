// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/config_menu.h — the emulator's settings menu, as a
// portable state machine: HID key presses in, a text screen out. The Pi
// front end draws the screen and supplies a directory lister (FatFs); the
// unit tests drive it directly.
//
// Pages:
//  - Main: memory, ROM file, the four ATA positions, floppy (shown as not
//    available: the Blue & White G3 has none), start-up disk, screen size,
//    colours, then "Save settings" and "Start the Mac" / "Resume".
//  - Drive: type (none / hard disk / CD-ROM), image (file picker), eject,
//    read-only (hard disks), new blank disk (hard disks: Left/Right picks
//    the size, Enter asks the front end to create it; Action::CreateDisk),
//    back.
//  - File picker: directories first, ".." to go up; Enter picks a file.
//  - WiFi network (Enter on "Network port"): WiFi on/off, "Search for
//    networks", the network's name, password and country typed in (Enter
//    starts typing, Enter ends, Esc cancels, Backspace deletes; key() then
//    needs the Shift state), and the keyboard's layout (US/UK) for typing
//    them.
//  - Networks found (after "Search for networks"): the menu returns
//    Action::ScanWifi and shows "Searching..."; the front end scans and
//    calls wifi_scan_done(). Enter on a network takes its name, turns
//    WiFi on and, for a WPA/WPA2 network, starts typing its password.
//
// Keys: Up/Down (and PageUp/PageDown/Home/End) move, Left/Right change a
// value, Enter opens or activates, Esc goes back (on the main page it means
// Start/Resume). Keys are USB HID usage IDs (Usage Tables 1.12, keyboard page).
//
// While the Mac is running (set_running(true)), changes that need a restart
// (memory, ROM, drive types, screen) are marked with '*' and reported by
// needs_restart(), and the last item reads "Restart the Mac"; inserting or ejecting a CD image takes effect at once
// (Action::MediaChanged, with media_changed_drive()).

#ifndef G3_PLATFORM_COMMON_CONFIG_MENU_H
#define G3_PLATFORM_COMMON_CONFIG_MENU_H

#include "core/common/types.h"
#include "core/machine/machine_config.h"
#include "platform/common/wifi_scan.h"

namespace g3 {

namespace menu_key {
constexpr u8 kEnter = 0x28, kEscape = 0x29, kBackspace = 0x2A, kSpace = 0x2C;
constexpr u8 kHome = 0x4A, kPageUp = 0x4B, kEnd = 0x4D, kPageDown = 0x4E;
constexpr u8 kRight = 0x4F, kLeft = 0x50, kDown = 0x51, kUp = 0x52;
constexpr u8 kF12 = 0x45;  // the front end's "open the menu" key
} // namespace menu_key

// Directory listing, supplied by the front end.
struct DirEntry {
    static constexpr u32 kNameMax = 96;
    char name[kNameMax] = {};
    bool directory = false;
    u64 size = 0;
};
class DirectoryLister {
public:
    // Fills up to `max` entries of `path` (no "." or ".."), any order.
    // Returns the number filled, or -1 if the directory cannot be read.
    virtual s32 list(const char* path, DirEntry* out, u32 max) = 0;

protected:
    ~DirectoryLister() = default;
};

class ConfigMenu {
public:
    static constexpr u32 kCols = 78, kRows = 30;
    static constexpr u32 kMaxFiles = 128;

    enum class Attr : u8 { Normal, Selected, Title, Dim, Status, Changed };
    struct Screen {
        char text[kRows][kCols + 1];
        Attr attr[kRows];
    };
    enum class Action : u8 { None, Save, Start, MediaChanged, SelfTest, CreateDisk, RestartPi, ScanWifi };

    // `disks_dir`/`rom_dir`: where the file pickers start (e.g. "/g3macpi/disks").
    ConfigMenu(const MachineConfig& config, DirectoryLister& lister, const char* disks_dir,
               const char* rom_dir);

    // A key press. Returns what the front end should do now. `modifiers`:
    // the HID modifier byte (Shift matters when text is being typed).
    Action key(u8 usage, u8 modifiers = 0);
    bool typing() const { return editing_ != kNotEditing; }

    void render(Screen& out) const;

    const MachineConfig& config() const { return cfg_; }
    void set_running(bool running);       // the Mac is running: restart-only changes are marked
    bool needs_restart() const;           // a restart-only setting differs from the running one
    bool dirty() const { return dirty_; } // changed since the last Save
    void mark_saved() { dirty_ = false; }
    u32 media_changed_drive() const { return media_drive_; }
    // A disc put in (path) or taken out ("") of a CD-ROM drive from outside
    // the menu (the web interface): as choosing it in the drive page.
    // MediaChanged when the running Mac must be told; None otherwise (also
    // when the drive is not a CD-ROM drive: ok = false then).
    Action insert_media(u32 drive, const char* path, bool& ok);
    void set_status(const char* message);  // one line under the menu (e.g. "Settings saved")
    // The running Mac ejected the disc in `drive` itself: forget its image.
    void disc_ejected(u32 drive);

    // Separate starting folders for CD images and hard-disk images (both
    // default to disks_dir). New blank disks are made in the hard-disk folder.
    void set_media_folders(const char* cd_dir, const char* hdd_dir)
    {
        cd_dir_ = cd_dir;
        hdd_dir_ = hdd_dir;
    }

    // New blank hard disk (after Action::CreateDisk): the file to create,
    // a free name in the hard-disk folder ("hd1.img", "hd2.img", ...), and its size.
    // Sizes stay below 4 GB, the largest file FAT32 holds.
    const char* new_disk_path() const { return new_disk_path_; }
    u64 new_disk_bytes() const;
    // The front end reports the result; on success the disk goes into the
    // drive being edited (as a writable hard disk). mountable: it carries
    // Apple's driver and a volume (Mac OS shows it); else it is blank and
    // message says why (Drive Setup must initialise it).
    void disk_created(bool ok, bool mountable, const char* message);
    static constexpr u32 kDiskSizes = 5;
    static u32 disk_size_mb(u32 index);  // 256, 512, 1024, 2048, 4095

    // After Action::ScanWifi: the networks found, or why the search failed
    // (error non-null; the list is then empty).
    void wifi_scan_done(const WifiScanList& found, const char* error);
    bool wifi_searching() const { return page_ == Page::WifiList && searching_; }

private:
    enum class Page : u8 { Main, Drive, Files, Wifi, WifiList };
    // In screen order, by group (The Mac, Drives, Screen, Network, The Pi),
    // then the actions. kFloppy is not shown (the B&W has no floppy drive).
    enum MainItem : u8 { kRam, kRom, kJit, kDrive0, kFloppy = kDrive0 + MachineConfig::kDrives, kBoot, kNewHardDisk,
                         kMode, kDepth, kScale, kSound, kNetwork, kWifi, kModem, kAutostart, kTimeZone, kPiSpeed, kSelfTest,
                         kSave, kStart, kRestartPi, kMainItems };
    enum DriveItem : u8 { kType, kImage, kEject, kReadOnly, kNewDisk, kBack, kDriveItems };
    enum WifiItem : u8 { kWUse, kWSearch, kWName, kWPassword, kWCountry, kWKeyboard, kWBack, kWifiItems };
    static constexpr u8 kNotEditing = 0xFF;

    Action key_main(u8 usage);
    Action key_drive(u8 usage);
    Action key_files(u8 usage);
    Action key_wifi(u8 usage);
    Action key_wifi_list(u8 usage);
    void pick_network(const WifiNetwork& n);
    bool list_enabled(u32) const { return true; }
    void key_edit(u8 usage, u8 modifiers);
    void start_edit(u8 item);
    bool wifi_enabled(u32) const { return true; }
    void toggle_wifi();
    void move(u8 usage, u32& sel, u32 count, bool (ConfigMenu::*enabled)(u32) const);
    bool main_enabled(u32 i) const;
    bool drive_enabled(u32 i) const;
    bool file_enabled(u32) const { return true; }
    void change_main(u32 item, int dir);
    void change_drive(u32 item, int dir);
    void open_files(bool rom);
    void load_dir();
    Action pick_file();
    Action set_drive_image(const char* path);
    Action request_new_disk();
    // A row: two spaces, `left`, and `right` starting at column `col`.
    void line(Screen& s, u32& row, Attr a, const char* left, const char* right = nullptr, u32 col = 26) const;
    static constexpr u32 kGroupWidth = 10, kLabelWidth = 24;  // main page columns
    bool restart_differs(u32 main_item) const;

    MachineConfig cfg_, running_cfg_;
    DirectoryLister& lister_;
    const char* disks_dir_;
    const char* rom_dir_;
    const char* cd_dir_;
    const char* hdd_dir_;
    Page page_ = Page::Main;
    u32 sel_ = 0, drive_sel_ = 0, file_sel_ = 0, wifi_sel_ = 0;
    u8 editing_ = kNotEditing;   // the WiFi item being typed
    char edit_[MachineConfig::kWifiPasswordMax] = {};
    mutable u32 file_top_ = 0;  // first visible file (render scrolls)
    WifiScanList networks_;     // the last search's results
    u32 list_sel_ = 0;          // networks, then "Search again", "Back"
    mutable u32 list_top_ = 0;
    bool searching_ = false;
    char scan_error_[kCols + 1] = {};
    u32 drive_ = 0;          // drive being edited
    bool picking_rom_ = false;
    bool running_ = false;
    bool dirty_ = false;
    u32 media_drive_ = 0;
    char status_[3 * kCols + 1] = {};  // shown on up to three lines
    char dir_[MachineConfig::kPathMax] = {};
    DirEntry files_[kMaxFiles];
    u32 nfiles_ = 0;
    bool has_parent_ = false;  // ".." shown as the first entry
    bool dir_error_ = false;
    u32 disk_size_ = 2;  // index into the sizes: 1 GB
    char new_disk_path_[MachineConfig::kPathMax] = {};
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_CONFIG_MENU_H
