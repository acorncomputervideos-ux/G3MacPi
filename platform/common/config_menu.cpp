// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/config_menu.cpp — see config_menu.h.

#include "platform/common/config_menu.h"
#include "platform/common/key_text.h"

namespace g3 {

namespace {

using DriveType = MachineConfig::DriveType;
using Boot = MachineConfig::Boot;

u32 slen(const char* s)
{
    u32 n = 0;
    while (s[n])
        ++n;
    return n;
}

bool seq(const char* a, const char* b)
{
    for (; *a && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

void scopy(char* dst, u32 cap, const char* src)
{
    u32 i = 0;
    for (; src[i] && i + 1 < cap; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

void scat(char* dst, u32 cap, const char* src)
{
    const u32 n = slen(dst);
    if (n < cap)
        scopy(dst + n, cap - n, src);
}

void sappend_num(char* dst, u32 cap, u64 v)
{
    char b[24];
    u32 n = 0;
    do {
        b[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    char out[24];
    for (u32 i = 0; i < n; ++i)
        out[i] = b[n - 1 - i];
    out[n] = 0;
    scat(dst, cap, out);
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive order; directories first.
bool before(const DirEntry& a, const DirEntry& b)
{
    if (a.directory != b.directory)
        return a.directory;
    const char* p = a.name;
    const char* q = b.name;
    for (; *p && lower(*p) == lower(*q); ++p, ++q) {
    }
    return lower(*p) < lower(*q);
}

// The last path component.
const char* base_name(const char* path)
{
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/')
            b = p + 1;
    return b;
}

const char* depth_name(u32 depth)
{
    return depth == 8 ? "256 colours" : depth == 16 ? "Thousands of colours" : "Millions of colours";
}

const char* boot_label(Boot b)
{
    return b == Boot::CdRom ? "CD-ROM" : b == Boot::Disk ? "Hard disk" : "Automatic";
}

// Index i moved by dir (+1/-1) among n choices, wrapping round.
u32 cycle(u32 i, u32 n, int dir) { return static_cast<u32>(static_cast<int>(i + n) + dir) % n; }

} // namespace

ConfigMenu::ConfigMenu(const MachineConfig& config, DirectoryLister& lister, const char* disks_dir,
                       const char* rom_dir)
    : cfg_(config), running_cfg_(config), lister_(lister), disks_dir_(disks_dir), rom_dir_(rom_dir), cd_dir_(disks_dir), hdd_dir_(disks_dir)
{
}

void ConfigMenu::set_running(bool running)
{
    running_ = running;
    running_cfg_ = cfg_;
}

void ConfigMenu::set_status(const char* message) { scopy(status_, sizeof status_, message); }

void ConfigMenu::disc_ejected(u32 drive)
{
    if (drive >= MachineConfig::kDrives || cfg_.drive[drive].type != DriveType::CdRom)
        return;
    cfg_.drive[drive].image[0] = 0;
    running_cfg_.drive[drive].image[0] = 0;
    dirty_ = true;
}

bool ConfigMenu::restart_differs(u32 item) const
{
    if (!running_)
        return false;
    const MachineConfig& a = cfg_;
    const MachineConfig& b = running_cfg_;
    if (item == kRam) return a.ram_mb != b.ram_mb;
    if (item == kRom) return !seq(a.rom, b.rom);
    if (item == kBoot) return a.boot != b.boot;
    if (item == kMode) return a.width != b.width || a.height != b.height;
    if (item == kDepth) return a.depth != b.depth;
    if (item == kSound) return a.sound != b.sound;
    if (item == kNetwork) return a.network != b.network;
    if (item == kModem) return a.modem != b.modem;
    if (item == kJit) return a.jit != b.jit;
    if (item >= kDrive0 && item < kFloppy) {
        const auto& x = a.drive[item - kDrive0];
        const auto& y = b.drive[item - kDrive0];
        if (x.type != y.type)
            return true;
        // A hard disk is never swapped under a running system.
        return x.type == DriveType::Disk && (!seq(x.image, y.image) || x.read_only != y.read_only);
    }
    return false;
}

bool ConfigMenu::needs_restart() const
{
    for (u32 i = 0; i < kMainItems; ++i)
        if (restart_differs(i))
            return true;
    return false;
}

// --- keys ----------------------------------------------------------------------------

ConfigMenu::Action ConfigMenu::key(u8 usage, u8 modifiers)
{
    if (editing_ != kNotEditing) {
        key_edit(usage, modifiers);
        return Action::None;
    }
    switch (page_) {
    case Page::Main: return key_main(usage);
    case Page::Drive: return key_drive(usage);
    case Page::Files: return key_files(usage);
    case Page::Wifi: return key_wifi(usage);
    }
    return Action::None;
}

// --- the WiFi page ---------------------------------------------------------------------

void ConfigMenu::toggle_wifi()
{
    cfg_.wifi = !cfg_.wifi;
    dirty_ = true;
    set_status("The network port changes when the Pi restarts: save the settings first.");
}

ConfigMenu::Action ConfigMenu::key_wifi(u8 usage)
{
    switch (usage) {
    case menu_key::kEscape:
    case menu_key::kBackspace:
        page_ = Page::Main;
        return Action::None;
    case menu_key::kLeft:
    case menu_key::kRight:
        if (wifi_sel_ == kWUse) {
            toggle_wifi();
        } else if (wifi_sel_ == kWKeyboard) {
            cfg_.keyboard = cfg_.keyboard == MachineConfig::Keyboard::Us ? MachineConfig::Keyboard::Uk
                                                                       : MachineConfig::Keyboard::Us;
            dirty_ = true;
        }
        return Action::None;
    case menu_key::kEnter:
    case menu_key::kSpace:
        if (wifi_sel_ == kWBack)
            page_ = Page::Main;
        else if (wifi_sel_ == kWName || wifi_sel_ == kWPassword || wifi_sel_ == kWCountry)
            start_edit(static_cast<u8>(wifi_sel_));
        else
            return key_wifi(menu_key::kRight);
        return Action::None;
    default:
        move(usage, wifi_sel_, kWifiItems, &ConfigMenu::wifi_enabled);
        return Action::None;
    }
}

void ConfigMenu::start_edit(u8 item)
{
    editing_ = item;
    scopy(edit_, sizeof edit_, item == kWName ? cfg_.wifi_name : item == kWPassword ? cfg_.wifi_password
                                                                                     : cfg_.wifi_country);
    status_[0] = 0;
}

void ConfigMenu::key_edit(u8 usage, u8 modifiers)
{
    const u32 max = editing_ == kWName ? MachineConfig::kWifiNameMax - 1
                    : editing_ == kWPassword ? MachineConfig::kWifiPasswordMax - 1 : 2;
    u32 len = slen(edit_);
    if (usage == menu_key::kEscape) {
        editing_ = kNotEditing;
        status_[0] = 0;
        return;
    }
    if (usage == menu_key::kBackspace) {
        if (len > 0)
            edit_[len - 1] = 0;
        return;
    }
    if (usage == menu_key::kEnter) {
        if (editing_ == kWPassword && len > 0 && len < 8) {
            set_status("A WiFi password has 8 to 63 characters (none for an open network).");
            return;
        }
        if (editing_ == kWCountry && len == 1) {
            set_status("A country code has two letters, e.g. GB, US, DE.");
            return;
        }
        char* dst = editing_ == kWName ? cfg_.wifi_name : editing_ == kWPassword ? cfg_.wifi_password
                                                                                : cfg_.wifi_country;
        const u32 cap = editing_ == kWName ? MachineConfig::kWifiNameMax
                        : editing_ == kWPassword ? MachineConfig::kWifiPasswordMax : 3;
        if (!seq(dst, edit_)) {
            scopy(dst, cap, edit_);
            dirty_ = true;
        }
        editing_ = kNotEditing;
        set_status("Changes to WiFi apply when the Pi restarts: save the settings first.");
        return;
    }
    const KeyLayout layout = cfg_.keyboard == MachineConfig::Keyboard::Uk ? KeyLayout::Uk : KeyLayout::Us;
    char c = key_char(usage, shift_held(modifiers), layout);
    if (c == 0)
        return;
    if (editing_ == kWCountry) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        if (c < 'A' || c > 'Z')
            return;
    }
    if (editing_ == kWPassword && c == '"') {
        set_status("A WiFi password here can't contain \" (the WPA supplicant's file format).");
        return;
    }
    if ((c == '#' || c == ';') && (len == 0 || edit_[len - 1] == ' ')) {
        // The settings file would read it as the start of a comment.
        set_status("# or ; can't come first or after a space (g3macpi.ini reads a comment there).");
        return;
    }
    if (len >= max)
        return;
    edit_[len] = c;
    edit_[len + 1] = 0;
}

void ConfigMenu::move(u8 usage, u32& sel, u32 count, bool (ConfigMenu::*enabled)(u32) const)
{
    if (count == 0)
        return;
    int step = 0, repeat = 1;
    switch (usage) {
    case menu_key::kUp: step = -1; break;
    case menu_key::kDown: step = 1; break;
    case menu_key::kPageUp: step = -1; repeat = 10; break;
    case menu_key::kPageDown: step = 1; repeat = 10; break;
    case menu_key::kHome: step = -1; repeat = static_cast<int>(count); break;
    case menu_key::kEnd: step = 1; repeat = static_cast<int>(count); break;
    default: return;
    }
    for (int r = 0; r < repeat; ++r) {
        u32 next = sel;
        // Skip disabled items; stop at the ends.
        do {
            if ((step < 0 && next == 0) || (step > 0 && next + 1 >= count))
                return;
            next = static_cast<u32>(static_cast<int>(next) + step);
        } while (!(this->*enabled)(next));
        sel = next;
    }
}

bool ConfigMenu::main_enabled(u32 i) const { return i != kFloppy; }

bool ConfigMenu::drive_enabled(u32 i) const
{
    const auto& d = cfg_.drive[drive_];
    if (i == kImage) return d.type != DriveType::None;
    if (i == kEject) return d.type != DriveType::None && d.image[0] != 0;
    if (i == kReadOnly || i == kNewDisk) return d.type == DriveType::Disk;
    return true;
}

ConfigMenu::Action ConfigMenu::key_main(u8 usage)
{
    switch (usage) {
    case menu_key::kLeft:
    case menu_key::kRight:
        change_main(sel_, usage == menu_key::kLeft ? -1 : 1);
        return Action::None;
    case menu_key::kEscape:
        return Action::Start;  // leave the menu: start or resume the Mac
    case menu_key::kEnter:
    case menu_key::kSpace:
        if (sel_ >= kDrive0 && sel_ < kFloppy) {
            drive_ = sel_ - kDrive0;
            drive_sel_ = kType;
            page_ = Page::Drive;
            return Action::None;
        }
        if (sel_ == kRom) {
            open_files(true);
            return Action::None;
        }
        if (sel_ == kWifi) {
            wifi_sel_ = kWUse;
            page_ = Page::Wifi;
            return Action::None;
        }
        if (sel_ == kSave)
            return Action::Save;
        if (sel_ == kStart)
            return Action::Start;
        if (sel_ == kSelfTest)
            return Action::SelfTest;
        if (sel_ == kRestartPi)
            return Action::RestartPi;
        if (sel_ == kNewHardDisk) {
            // The first free drive position, hard disk bus first (the
            // firmware starts from the master positions): made a hard disk,
            // its page opened on "New blank disk".
            static const u32 kOrder[4] = {2, 3, 1, 0};
            for (u32 k = 0; k < 4; ++k) {
                auto& d = cfg_.drive[kOrder[k]];
                if (d.type == DriveType::None || (d.type == DriveType::Disk && !d.image[0])) {
                    if (d.type != DriveType::Disk) {
                        d.type = DriveType::Disk;
                        d.read_only = false;
                        dirty_ = true;
                    }
                    drive_ = kOrder[k];
                    drive_sel_ = kNewDisk;
                    page_ = Page::Drive;
                    return Action::None;
                }
            }
            set_status("All four drive positions are in use: open one, set it to Hard disk, then New blank disk.");
            return Action::None;
        }
        change_main(sel_, 1);
        return Action::None;
    default:
        move(usage, sel_, kMainItems, &ConfigMenu::main_enabled);
        return Action::None;
    }
}

void ConfigMenu::change_main(u32 item, int dir)
{
    if (item == kRam) {
        const u32 n = sizeof MachineConfig::kRamChoices / sizeof MachineConfig::kRamChoices[0];
        u32 i = 0;
        while (i < n && MachineConfig::kRamChoices[i] != cfg_.ram_mb)
            ++i;
        i = (i >= n) ? 0 : cycle(i, n, dir);
        cfg_.ram_mb = MachineConfig::kRamChoices[i];
    } else if (item == kBoot) {
        const Boot order[3] = {Boot::Auto, Boot::CdRom, Boot::Disk};
        u32 i = 0;
        while (order[i] != cfg_.boot)
            ++i;
        cfg_.boot = order[cycle(i, 3, dir)];
    } else if (item == kMode) {
        const u32 n = sizeof MachineConfig::kModes / sizeof MachineConfig::kModes[0];
        u32 i = 0;
        while (i < n && (MachineConfig::kModes[i].width != cfg_.width || MachineConfig::kModes[i].height != cfg_.height))
            ++i;
        i = (i >= n) ? 0 : cycle(i, n, dir);
        cfg_.width = MachineConfig::kModes[i].width;
        cfg_.height = MachineConfig::kModes[i].height;
    } else if (item == kDepth) {
        const u32 n = sizeof MachineConfig::kDepths / sizeof MachineConfig::kDepths[0];
        u32 i = 0;
        while (i < n && MachineConfig::kDepths[i] != cfg_.depth)
            ++i;
        i = (i >= n) ? 0 : cycle(i, n, dir);
        cfg_.depth = MachineConfig::kDepths[i];
    } else if (item == kSound) {
        cfg_.sound = !cfg_.sound;
    } else if (item == kNetwork) {
        cfg_.network = !cfg_.network;
    } else if (item == kWifi) {
        toggle_wifi();
        return;
    } else if (item == kModem) {
        cfg_.modem = !cfg_.modem;
    } else if (item == kAutostart) {
        cfg_.autostart = !cfg_.autostart;
    } else if (item == kJit) {
        cfg_.jit = !cfg_.jit;
    } else if (item == kPiSpeed) {
        cfg_.pi_overclock = !cfg_.pi_overclock;
        set_status(cfg_.pi_overclock
                       ? "2.0 GHz: written to config.txt by Save settings; applies when the Pi restarts. "
                         "If the Pi becomes unstable, set it back."
                       : "1.8 GHz: written to config.txt by Save settings; applies when the Pi restarts.");
    } else {
        return;
    }
    dirty_ = true;
}

ConfigMenu::Action ConfigMenu::key_drive(u8 usage)
{
    switch (usage) {
    case menu_key::kEscape:
    case menu_key::kBackspace:
        page_ = Page::Main;
        return Action::None;
    case menu_key::kLeft:
    case menu_key::kRight:
        change_drive(drive_sel_, usage == menu_key::kLeft ? -1 : 1);
        return Action::None;
    case menu_key::kEnter:
    case menu_key::kSpace:
        if (drive_sel_ == kBack) {
            page_ = Page::Main;
            return Action::None;
        }
        if (drive_sel_ == kImage) {
            open_files(false);
            return Action::None;
        }
        if (drive_sel_ == kEject)
            return set_drive_image("");
        if (drive_sel_ == kNewDisk)
            return request_new_disk();
        change_drive(drive_sel_, 1);
        return Action::None;
    default:
        move(usage, drive_sel_, kDriveItems, &ConfigMenu::drive_enabled);
        return Action::None;
    }
}

void ConfigMenu::change_drive(u32 item, int dir)
{
    auto& d = cfg_.drive[drive_];
    if (item == kType) {
        const DriveType order[3] = {DriveType::None, DriveType::Disk, DriveType::CdRom};
        u32 i = 0;
        while (order[i] != d.type)
            ++i;
        d.type = order[cycle(i, 3, dir)];
        d.image[0] = 0;  // an image of one kind is not media for the other
        d.read_only = d.type == DriveType::CdRom;
    } else if (item == kReadOnly && d.type == DriveType::Disk) {
        d.read_only = !d.read_only;
    } else if (item == kNewDisk && d.type == DriveType::Disk) {
        disk_size_ = cycle(disk_size_, kDiskSizes, dir);
        return;  // only the size to create changed, not the settings
    } else {
        return;
    }
    dirty_ = true;
}

u32 ConfigMenu::disk_size_mb(u32 index)
{
    static const u32 kSizes[kDiskSizes] = {256, 512, 1024, 2048, 4095};
    return kSizes[index < kDiskSizes ? index : 0];
}

u64 ConfigMenu::new_disk_bytes() const { return static_cast<u64>(disk_size_mb(disk_size_)) << 20; }

// A free name in disks_dir: hd1.img, hd2.img, ... (case-insensitive check,
// FAT names are). Nothing is created here: the front end does that.
ConfigMenu::Action ConfigMenu::request_new_disk()
{
    s32 n = lister_.list(hdd_dir_, files_, kMaxFiles);
    if (n < 0) {
        set_status("The hard-disk folder cannot be read.");
        return Action::None;
    }
    char name[16];
    for (u32 k = 1; k < 1000; ++k) {
        char num[8];
        u32 len = 0;
        for (u32 v = k; v; v /= 10)
            num[len++] = static_cast<char>('0' + v % 10);
        u32 j = 0;
        name[j++] = 'h';
        name[j++] = 'd';
        while (len)
            name[j++] = num[--len];
        name[j++] = '.';
        name[j++] = 'i';
        name[j++] = 'm';
        name[j++] = 'g';
        name[j] = 0;
        bool used = false;
        for (s32 i = 0; i < n && !used; ++i) {
            const char* a = files_[i].name;
            const char* b = name;
            while (*a && *b && (*a | 0x20) == (*b | 0x20)) {
                ++a;
                ++b;
            }
            used = *a == 0 && *b == 0;
        }
        if (used)
            continue;
        scopy(new_disk_path_, sizeof new_disk_path_, hdd_dir_);
        scat(new_disk_path_, sizeof new_disk_path_, "/");
        scat(new_disk_path_, sizeof new_disk_path_, name);
        set_status("Creating the disk...");
        return Action::CreateDisk;
    }
    set_status("No free disk name (hd1.img ... hd999.img all exist).");
    return Action::None;
}

void ConfigMenu::disk_created(bool ok, const char* message)
{
    if (!ok) {
        set_status(message && message[0] ? message : "The disk could not be created.");
        return;
    }
    auto& d = cfg_.drive[drive_];
    d.type = DriveType::Disk;
    d.read_only = false;
    set_drive_image(new_disk_path_);
    char buf[kCols + 1];
    scopy(buf, sizeof buf, "Created ");
    scat(buf, sizeof buf, new_disk_path_);
    scat(buf, sizeof buf, ": initialise it in Mac OS with Drive Setup.");
    set_status(message && message[0] ? message : buf);
}

ConfigMenu::Action ConfigMenu::set_drive_image(const char* path)
{
    auto& d = cfg_.drive[drive_];
    if (!cfg_.set_image(drive_, path)) {
        set_status("That path is too long.");
        return Action::None;
    }
    dirty_ = true;
    if (!drive_enabled(drive_sel_))
        drive_sel_ = kImage;
    if (running_ && d.type == DriveType::CdRom && running_cfg_.drive[drive_].type == DriveType::CdRom) {
        // A CD is inserted or ejected in the running Mac at once.
        scopy(running_cfg_.drive[drive_].image, MachineConfig::kPathMax, path);
        media_drive_ = drive_;
        return Action::MediaChanged;
    }
    return Action::None;
}

ConfigMenu::Action ConfigMenu::insert_media(u32 drive, const char* path, bool& ok)
{
    ok = drive < MachineConfig::kDrives && cfg_.drive[drive].type == DriveType::CdRom;
    if (!ok)
        return Action::None;
    const u32 saved = drive_;
    drive_ = drive;
    const Action a = set_drive_image(path);
    drive_ = saved;
    ok = seq(cfg_.drive[drive].image, path);
    return a;
}

// --- file picker ---------------------------------------------------------------------

void ConfigMenu::open_files(bool rom)
{
    picking_rom_ = rom;
    // Start in the folder of the current file, else the default folder.
    const char* current = rom ? cfg_.rom : cfg_.drive[drive_].image;
    if (current[0] == '/' && base_name(current) != current + 1) {
        scopy(dir_, sizeof dir_, current);
        dir_[base_name(current) - current - 1] = 0;
    } else {
        scopy(dir_, sizeof dir_,
              rom ? rom_dir_ : cfg_.drive[drive_].type == DriveType::CdRom ? cd_dir_ : hdd_dir_);
    }
    page_ = Page::Files;
    load_dir();
    // Select the current file if it is there.
    const char* name = base_name(current);
    for (u32 i = 0; i < nfiles_; ++i)
        if (seq(files_[i].name, name))
            file_sel_ = i + (has_parent_ ? 1 : 0);
}

void ConfigMenu::load_dir()
{
    has_parent_ = !seq(dir_, "/") && dir_[0] != 0;
    const s32 n = lister_.list(dir_[0] ? dir_ : "/", files_, kMaxFiles);
    dir_error_ = n < 0;
    nfiles_ = n < 0 ? 0 : static_cast<u32>(n);
    // Insertion sort (small lists).
    for (u32 i = 1; i < nfiles_; ++i) {
        DirEntry e = files_[i];
        u32 j = i;
        for (; j > 0 && before(e, files_[j - 1]); --j)
            files_[j] = files_[j - 1];
        files_[j] = e;
    }
    file_sel_ = 0;
    file_top_ = 0;
}

ConfigMenu::Action ConfigMenu::key_files(u8 usage)
{
    const u32 count = nfiles_ + (has_parent_ ? 1 : 0);
    switch (usage) {
    case menu_key::kEscape:
        page_ = picking_rom_ ? Page::Main : Page::Drive;
        return Action::None;
    case menu_key::kBackspace:
        if (!has_parent_)
            return Action::None;
        file_sel_ = 0;
        [[fallthrough]];
    case menu_key::kEnter:
    case menu_key::kSpace:
        if (count == 0)
            return Action::None;
        return pick_file();
    default:
        move(usage, file_sel_, count, &ConfigMenu::file_enabled);
        return Action::None;
    }
}

ConfigMenu::Action ConfigMenu::pick_file()
{
    if (has_parent_ && file_sel_ == 0) {
        // "..": drop the last component.
        char* slash = nullptr;
        for (char* p = dir_; *p; ++p)
            if (*p == '/')
                slash = p;
        if (slash == dir_)
            dir_[1] = 0;
        else if (slash)
            *slash = 0;
        load_dir();
        return Action::None;
    }
    const DirEntry& e = files_[file_sel_ - (has_parent_ ? 1 : 0)];
    char path[MachineConfig::kPathMax];
    scopy(path, sizeof path, dir_);
    if (!(path[0] == '/' && path[1] == 0))
        scat(path, sizeof path, "/");
    scat(path, sizeof path, e.name);
    if (slen(path) != slen(dir_) + slen(e.name) + (seq(dir_, "/") ? 0 : 1)) {
        set_status("That path is too long.");
        return Action::None;
    }
    if (e.directory) {
        scopy(dir_, sizeof dir_, path);
        load_dir();
        return Action::None;
    }
    if (picking_rom_) {
        scopy(cfg_.rom, MachineConfig::kPathMax, path);
        dirty_ = true;
        page_ = Page::Main;
        return Action::None;
    }
    page_ = Page::Drive;
    return set_drive_image(path);
}

// --- drawing -------------------------------------------------------------------------

void ConfigMenu::line(Screen& s, u32& row, Attr a, const char* left, const char* right, u32 col) const
{
    if (row >= kRows)
        return;
    char* t = s.text[row];
    u32 n = 0;
    auto put = [&](const char* p) {
        for (; *p && n < kCols; ++p)
            t[n++] = *p;
    };
    put("  ");
    put(left);
    if (right) {
        if (n >= col && n < kCols)
            t[n++] = ' ';  // always at least one blank before the value
        while (n < col && n < kCols)
            t[n++] = ' ';
        put(right);
    }
    t[n] = 0;
    s.attr[row] = a;
    ++row;
}

void ConfigMenu::render(Screen& s) const
{
    for (u32 r = 0; r < kRows; ++r) {
        s.text[r][0] = 0;
        s.attr[r] = Attr::Normal;
    }
    u32 row = 0;
    char buf[kCols + 1];
    auto item_attr = [&](bool selected, bool changed) {
        return selected ? Attr::Selected : changed ? Attr::Changed : Attr::Normal;
    };

    if (page_ == Page::Main) {
        line(s, row, Attr::Title, "G3MacPi settings - Power Macintosh G3 (Blue & White)");
        ++row;
        for (u32 i = 0; i < kMainItems; ++i) {
            const bool sel = i == sel_;
            const bool chg = restart_differs(i);
            buf[0] = 0;
            char label[40];
            scopy(label, sizeof label, chg ? "*" : "");
            if (i == kRam) {
                sappend_num(buf, sizeof buf, cfg_.ram_mb);
                scat(buf, sizeof buf, " MB");
                scat(label, sizeof label, "Memory");
            } else if (i == kRom) {
                scopy(buf, sizeof buf, cfg_.rom);
                scat(label, sizeof label, "ROM file");
            } else if (i >= kDrive0 && i < kFloppy) {
                const auto& d = cfg_.drive[i - kDrive0];
                scat(label, sizeof label, MachineConfig::drive_label(i - kDrive0));
                if (d.type == DriveType::None) {
                    scopy(buf, sizeof buf, "(no drive)");
                } else {
                    scopy(buf, sizeof buf, d.type == DriveType::CdRom ? "CD-ROM: " : "Hard disk: ");
                    scat(buf, sizeof buf, d.image[0] ? base_name(d.image) : "(empty)");
                    if (d.type == DriveType::Disk && d.read_only)
                        scat(buf, sizeof buf, " [read-only]");
                }
            } else if (i == kFloppy) {
                line(s, row, Attr::Dim, "Floppy", "none - the Blue & White G3 has no floppy drive");
                continue;
            } else if (i == kBoot) {
                scat(label, sizeof label, "Start-up disk");
                scopy(buf, sizeof buf, boot_label(cfg_.boot));
            } else if (i == kMode) {
                scat(label, sizeof label, "Screen");
                sappend_num(buf, sizeof buf, cfg_.width);
                scat(buf, sizeof buf, " x ");
                sappend_num(buf, sizeof buf, cfg_.height);
            } else if (i == kDepth) {
                scat(label, sizeof label, "Colours");
                scopy(buf, sizeof buf, depth_name(cfg_.depth));
            } else if (i == kSound) {
                scat(label, sizeof label, "Sound");
                scopy(buf, sizeof buf, cfg_.sound ? "On (HDMI)" : "Off");
            } else if (i == kNetwork) {
                scat(label, sizeof label, "Ethernet");
                scopy(buf, sizeof buf, !cfg_.network ? "Off" : cfg_.wifi ? "On (through the Pi's WiFi)"
                                                                      : "On (the Pi's Ethernet port)");
            } else if (i == kWifi) {
                scat(label, sizeof label, "Network port");
                if (!cfg_.wifi) {
                    scopy(buf, sizeof buf, "Ethernet");
                } else if (cfg_.wifi_name[0]) {
                    scopy(buf, sizeof buf, "WiFi: ");
                    scat(buf, sizeof buf, cfg_.wifi_name);
                } else {
                    scopy(buf, sizeof buf, "WiFi (no network set: wifi_name in g3macpi.ini)");
                }
            } else if (i == kModem) {
                scat(label, sizeof label, "Modem");
                scopy(buf, sizeof buf, cfg_.modem ? "Internal 56K: dials up through the network" : "None");
            } else if (i == kAutostart) {
                scat(label, sizeof label, "At power-on");
                scopy(buf, sizeof buf, cfg_.autostart ? "Start the Mac (F12: this menu)" : "Show this menu");
            } else if (i == kJit) {
                scat(label, sizeof label, "CPU emulation");
                scopy(buf, sizeof buf, cfg_.jit ? "JIT: faster (new)" : "Interpreter: safe, slower");
            } else if (i == kPiSpeed) {
                scat(label, sizeof label, "Pi CPU speed");
                scopy(buf, sizeof buf, cfg_.pi_overclock ? "2.0 GHz (overclock, from the next Pi restart)"
                                                         : "1.8 GHz (standard)");
            } else if (i == kSave) {
                ++row;
                line(s, row, item_attr(sel, false), dirty_ ? "Save settings (unsaved changes)" : "Save settings");
                continue;
            } else if (i == kStart) {
                line(s, row, item_attr(sel, false),
                     !running_ ? "Start the Mac"
                     : needs_restart() ? "Restart the Mac (applies the * changes)" : "Resume the Mac");
                continue;
            } else if (i == kSelfTest) {
                line(s, row, item_attr(sel, false), "CPU self-test (checks the JIT, measures speed)");
                continue;
            } else if (i == kNewHardDisk) {
                line(s, row, item_attr(sel, false), "New hard disk... (creates a blank disk image)");
                continue;
            } else if (i == kRestartPi) {
                line(s, row, item_attr(sel, false),
                     dirty_ ? "Restart the Pi (the Mac's disks are saved; unsaved settings are lost)"
                            : "Restart the Pi (the Mac's disks are saved first)");
                continue;
            }
            line(s, row, item_attr(sel, chg), label, buf);
        }
    } else if (page_ == Page::Drive) {
        const auto& d = cfg_.drive[drive_];
        scopy(buf, sizeof buf, "Drive: ");
        scat(buf, sizeof buf, MachineConfig::drive_label(drive_));
        line(s, row, Attr::Title, buf);
        ++row;
        for (u32 i = 0; i < kDriveItems; ++i) {
            const bool sel = i == drive_sel_;
            const Attr a = drive_enabled(i) ? item_attr(sel, false) : Attr::Dim;
            if (i == kType) {
                line(s, row, a, "Type", d.type == DriveType::None ? "No drive"
                                        : d.type == DriveType::Disk ? "Hard disk" : "CD-ROM");
            } else if (i == kImage) {
                line(s, row, a, d.type == DriveType::CdRom ? "Disc image" : "Disk image",
                     d.image[0] ? d.image : "(none) - press Enter to choose");
            } else if (i == kEject) {
                line(s, row, a, d.type == DriveType::CdRom ? "Eject disc" : "Remove image");
            } else if (i == kReadOnly) {
                line(s, row, a, "Read-only", d.type == DriveType::CdRom ? "yes (CD-ROM)" : d.read_only ? "yes" : "no");
            } else if (i == kNewDisk) {
                const u32 mb = disk_size_mb(disk_size_);
                char size[64];
                u32 j = 0;
                char num[8];
                u32 len = 0;
                const u32 v0 = mb >= 1024 ? (mb + 1) / 1024 : mb;
                for (u32 v = v0; v; v /= 10)
                    num[len++] = static_cast<char>('0' + v % 10);
                while (len)
                    size[j++] = num[--len];
                size[j] = 0;
                scat(size, sizeof size, mb >= 1024 ? " GB" : " MB");
                scat(size, sizeof size, "  (Left/Right: size, Enter: create)");
                line(s, row, a, "New blank disk", size);
            } else {
                ++row;
                line(s, row, a, "Back");
            }
        }
    } else if (page_ == Page::Wifi) {
        line(s, row, Attr::Title, "WiFi network");
        ++row;
        for (u32 i = 0; i < kWifiItems; ++i) {
            const bool sel = i == wifi_sel_;
            const bool typing_here = editing_ == i;
            const Attr a = item_attr(sel, false);
            char value[kCols + 1];
            if (typing_here) {
                scopy(value, sizeof value, edit_);
                scat(value, sizeof value, "_");
            }
            if (i == kWUse) {
                line(s, row, a, "Use WiFi", cfg_.wifi ? "Yes: the Pi's WiFi instead of its Ethernet port"
                                                       : "No: the Pi's Ethernet port");
            } else if (i == kWName) {
                line(s, row, a, "Network name",
                     typing_here ? value : cfg_.wifi_name[0] ? cfg_.wifi_name : "(none) - press Enter to type it");
            } else if (i == kWPassword) {
                if (!typing_here) {
                    const u32 n = slen(cfg_.wifi_password);
                    if (n == 0) {
                        scopy(value, sizeof value, "(none: an open network)");
                    } else {
                        for (u32 k = 0; k < n; ++k)
                            value[k] = '*';
                        value[n] = 0;
                    }
                }
                line(s, row, a, "Password", value);
            } else if (i == kWCountry) {
                line(s, row, a, "Country", typing_here ? value : cfg_.wifi_country[0] ? cfg_.wifi_country
                                                                       : "(not set) - e.g. GB, US");
            } else if (i == kWKeyboard) {
                line(s, row, a, "Keyboard (for typing)",
                     cfg_.keyboard == MachineConfig::Keyboard::Uk ? "UK" : "US");
            } else {
                ++row;
                line(s, row, a, "Back");
            }
        }
        ++row;
        line(s, row, Attr::Dim, "WPA2 or open networks. The password shows while you type it.");
        line(s, row, Attr::Dim, "Changes apply when the Pi restarts (Save settings on the main page).");
    } else {
        line(s, row, Attr::Title, picking_rom_ ? "Choose the Mac OS ROM file" : "Choose an image file");
        line(s, row, Attr::Dim, dir_);
        ++row;
        const u32 count = nfiles_ + (has_parent_ ? 1 : 0);
        const u32 visible = kRows - row - 4;
        u32 top = file_top_;
        if (file_sel_ < top) top = file_sel_;
        if (file_sel_ >= top + visible) top = file_sel_ - visible + 1;
        file_top_ = top;  // the scroll position follows the selection
        if (dir_error_)
            line(s, row, Attr::Dim, "(this folder cannot be read)");
        else if (count == 0)
            line(s, row, Attr::Dim, "(empty folder)");
        constexpr u32 kNameCol = 56, kNameMax = kNameCol - 4;  // names up to 52 characters
        for (u32 i = top; i < count && i < top + visible; ++i) {
            const Attr a = i == file_sel_ ? Attr::Selected : Attr::Normal;
            if (has_parent_ && i == 0) {
                line(s, row, a, "..", "(up one folder)", kNameCol);
                continue;
            }
            const DirEntry& e = files_[i - (has_parent_ ? 1 : 0)];
            buf[0] = 0;
            if (e.directory) {
                scopy(buf, sizeof buf, "(folder)");
            } else if (e.size >= 1024 * 1024) {
                sappend_num(buf, sizeof buf, e.size / (1024 * 1024));
                scat(buf, sizeof buf, " MB");
            } else {
                sappend_num(buf, sizeof buf, (e.size + 1023) / 1024);
                scat(buf, sizeof buf, " KB");
            }
            char name[kNameMax + 1];  // longer names end in "..."
            scopy(name, sizeof name, e.name);
            if (slen(e.name) > kNameMax)
                scopy(name + kNameMax - 3, 4, "...");
            line(s, row, a, name, buf, kNameCol);
        }
    }

    // Status and help at the bottom. A long status wraps (at spaces) onto
    // up to three lines, upwards, where those rows are free.
    row = kRows - 3;
    if (status_[0]) {
        char parts[3][kCols + 1] = {};
        u32 n = 0;
        const u32 width = kCols - 2;
        const char* p = status_;
        while (*p && n < 3) {
            u32 len = slen(p);
            if (len > width && n < 2) {
                len = width;
                while (len > 0 && p[len] != ' ')
                    --len;
                if (len == 0)
                    len = width;
            }
            if (len > width)
                len = width;
            for (u32 i = 0; i < len; ++i)
                parts[n][i] = p[i];
            parts[n][len] = 0;
            ++n;
            p += len;
            while (*p == ' ')
                ++p;
        }
        u32 first = kRows - 3 - (n - 1);
        for (u32 r = first; r < kRows - 3; ++r)
            for (const char* t = s.text[r]; *t; ++t)
                if (*t != ' ')
                    first = kRows - 3;  // taken: one line only
        if (first == kRows - 3)
            n = 1;
        u32 r = first;
        for (u32 k = 0; k < n; ++k)
            line(s, r, Attr::Status, parts[k]);
        row = kRows - 2;
    } else if (running_ && needs_restart())
        line(s, row, Attr::Status, "* takes effect when the Mac restarts");
    else
        ++row;
    ++row;
    line(s, row, Attr::Dim, editing_ != kNotEditing ? "Type the text   Backspace: delete   Enter: done   Esc: cancel"
                            : page_ == Page::Files ? "Up/Down: select   Enter: open/choose   Esc: cancel"
                            : "Up/Down: select   Left/Right: change   Enter: open   Esc: back");
}

} // namespace g3
