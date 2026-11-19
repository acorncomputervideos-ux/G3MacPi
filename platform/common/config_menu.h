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
//    read-only (hard disks), back.
//  - File picker: directories first, ".." to go up; Enter picks a file.
//
// Keys: Up/Down (and PageUp/PageDown/Home/End) move, Left/Right change a
// value, Enter opens or activates, Esc goes back (on the main page it means
// Start/Resume). Keys are USB HID usage IDs (Usage Tables 1.12, keyboard page).
//
// While the Mac is running (set_running(true)), changes that need a restart
// (memory, ROM, drive types, screen) are marked with '*' and reported by
// needs_restart(); inserting or ejecting a CD image takes effect at once
// (Action::MediaChanged, with media_changed_drive()).

#ifndef G3_PLATFORM_COMMON_CONFIG_MENU_H
#define G3_PLATFORM_COMMON_CONFIG_MENU_H

#include "core/common/types.h"
#include "core/machine/machine_config.h"

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
    enum class Action : u8 { None, Save, Start, MediaChanged };

    // `disks_dir`/`rom_dir`: where the file pickers start (e.g. "/g3macpi/disks").
    ConfigMenu(const MachineConfig& config, DirectoryLister& lister, const char* disks_dir,
               const char* rom_dir);

    // A key press. Returns what the front end should do now.
    Action key(u8 usage);

    void render(Screen& out) const;

    const MachineConfig& config() const { return cfg_; }
    void set_running(bool running);       // the Mac is running: restart-only changes are marked
    bool needs_restart() const;           // a restart-only setting differs from the running one
    bool dirty() const { return dirty_; } // changed since the last Save
    void mark_saved() { dirty_ = false; }
    u32 media_changed_drive() const { return media_drive_; }
    void set_status(const char* message);  // one line under the menu (e.g. "Settings saved")

private:
    enum class Page : u8 { Main, Drive, Files };
    enum MainItem : u8 { kRam, kRom, kDrive0, kFloppy = kDrive0 + MachineConfig::kDrives, kBoot, kMode,
                         kDepth, kSave, kStart, kMainItems };
    enum DriveItem : u8 { kType, kImage, kEject, kReadOnly, kBack, kDriveItems };

    Action key_main(u8 usage);
    Action key_drive(u8 usage);
    Action key_files(u8 usage);
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
    void line(Screen& s, u32& row, Attr a, const char* left, const char* right = nullptr) const;
    bool restart_differs(u32 main_item) const;

    MachineConfig cfg_, running_cfg_;
    DirectoryLister& lister_;
    const char* disks_dir_;
    const char* rom_dir_;
    Page page_ = Page::Main;
    u32 sel_ = 0, drive_sel_ = 0, file_sel_ = 0;
    mutable u32 file_top_ = 0;  // first visible file (render scrolls)
    u32 drive_ = 0;          // drive being edited
    bool picking_rom_ = false;
    bool running_ = false;
    bool dirty_ = false;
    u32 media_drive_ = 0;
    char status_[kCols + 1] = {};
    char dir_[MachineConfig::kPathMax] = {};
    DirEntry files_[kMaxFiles];
    u32 nfiles_ = 0;
    bool has_parent_ = false;  // ".." shown as the first entry
    bool dir_error_ = false;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_CONFIG_MENU_H
