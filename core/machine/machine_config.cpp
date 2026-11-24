// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/machine_config.cpp — see machine_config.h.

#include "core/machine/machine_config.h"

namespace g3 {

constexpr u32 MachineConfig::kRamChoices[];
constexpr MachineConfig::Mode MachineConfig::kModes[];
constexpr u32 MachineConfig::kDepths[];

namespace {

u32 length(const char* s)
{
    u32 n = 0;
    while (s[n])
        ++n;
    return n;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive comparison of [p, p + n) with a zero-terminated word.
bool word_is(const char* p, u32 n, const char* w)
{
    if (length(w) != n)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (lower(p[i]) != lower(w[i]))
            return false;
    return true;
}

bool copy(char* dst, u32 cap, const char* src, u32 n)
{
    if (n + 1 > cap)
        return false;
    for (u32 i = 0; i < n; ++i)
        dst[i] = src[i];
    dst[n] = 0;
    return true;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Unsigned decimal; false if empty, not a number or over 9 digits.
bool parse_u32(const char* p, u32 n, u32& out)
{
    if (n == 0 || n > 9)
        return false;
    u32 v = 0;
    for (u32 i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9')
            return false;
        v = v * 10 + static_cast<u32>(p[i] - '0');
    }
    out = v;
    return true;
}

bool parse_bool(const char* p, u32 n, bool& out)
{
    if (word_is(p, n, "yes") || word_is(p, n, "true") || word_is(p, n, "on") || word_is(p, n, "1")) {
        out = true;
        return true;
    }
    if (word_is(p, n, "no") || word_is(p, n, "false") || word_is(p, n, "off") || word_is(p, n, "0")) {
        out = false;
        return true;
    }
    return false;
}

// "WIDTHxHEIGHTxDEPTH"
bool parse_mode(const char* p, u32 n, u32& w, u32& h, u32& d)
{
    u32 field[3] = {}, f = 0, start = 0;
    for (u32 i = 0; i <= n; ++i) {
        if (i == n || p[i] == 'x' || p[i] == 'X') {
            if (f >= 3 || !parse_u32(p + start, i - start, field[f]))
                return false;
            ++f;
            start = i + 1;
        }
    }
    if (f != 3)
        return false;
    w = field[0];
    h = field[1];
    d = field[2];
    return true;
}

void reject(ConfigParseReport& r, u32 line, const char* reason)
{
    if (r.ignored++ == 0) {
        r.first_line = line;
        const u32 n = length(reason);
        copy(r.first_reason, sizeof r.first_reason, reason, n < sizeof r.first_reason - 1 ? n : sizeof r.first_reason - 1);
    }
}

// Output buffer for the writer.
struct Out {
    char* p;
    u32 cap, len = 0;
    bool ok = true;
    void ch(char c)
    {
        if (len + 1 >= cap) {
            ok = false;
            return;
        }
        p[len++] = c;
    }
    void str(const char* s)
    {
        for (; *s; ++s)
            ch(*s);
    }
    void num(u32 v)
    {
        char b[12];
        u32 n = 0;
        do {
            b[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (n)
            ch(b[--n]);
    }
};

enum Section : u8 { kNone, kMachine, kDrive0, kFloppy = kDrive0 + MachineConfig::kDrives, kUnknown };

} // namespace

MachineConfig::MachineConfig()
{
    drive[0].type = DriveType::CdRom;  // the B&W's CD-ROM position
    drive[0].read_only = true;
}

const char* MachineConfig::drive_name(u32 i)
{
    static const char* const names[kDrives] = {"ata0.master", "ata0.slave", "ata1.master", "ata1.slave"};
    return i < kDrives ? names[i] : "";
}

const char* MachineConfig::drive_label(u32 i)
{
    static const char* const labels[kDrives] = {"CD bus master", "CD bus slave", "Hard disk bus master",
                                                 "Hard disk bus slave"};
    return i < kDrives ? labels[i] : "";
}

const char* MachineConfig::type_name(DriveType t)
{
    return t == DriveType::Disk ? "disk" : t == DriveType::CdRom ? "cdrom" : "none";
}

const char* MachineConfig::boot_name(Boot b)
{
    return b == Boot::CdRom ? "cdrom" : b == Boot::Disk ? "disk" : "auto";
}

bool MachineConfig::valid_ram(u32 mb)
{
    for (u32 c : kRamChoices)
        if (c == mb)
            return true;
    return false;
}

bool MachineConfig::valid_mode(u32 w, u32 h, u32 d)
{
    bool depth_ok = false;
    for (u32 c : kDepths)
        depth_ok = depth_ok || c == d;
    if (!depth_ok)
        return false;
    for (const Mode& m : kModes)
        if (m.width == w && m.height == h)
            return true;
    return false;
}

bool MachineConfig::set_image(u32 i, const char* path)
{
    if (i >= kDrives)
        return false;
    return copy(drive[i].image, kPathMax, path, length(path));
}

void parse_machine_config(const char* text, u32 len, MachineConfig& c, ConfigParseReport& r)
{
    u32 section = kNone, line_no = 0, pos = 0;
    while (pos < len) {
        // One line: [b, e), without the newline.
        const u32 b0 = pos;
        while (pos < len && text[pos] != '\n')
            ++pos;
        u32 b = b0, e = pos;
        ++pos;
        ++line_no;
        // Comments: a line starting with ';' or '#', or one preceded by blanks.
        for (u32 i = b; i < e; ++i) {
            if ((text[i] == ';' || text[i] == '#') && (i == b || is_space(text[i - 1]))) {
                e = i;
                break;
            }
        }
        while (b < e && is_space(text[b]))
            ++b;
        while (e > b && is_space(text[e - 1]))
            --e;
        if (b == e)
            continue;
        const char* p = text + b;
        const u32 n = e - b;

        if (p[0] == '[') {
            if (p[n - 1] != ']') {
                reject(r, line_no, "malformed section header");
                section = kUnknown;
                continue;
            }
            const char* s = p + 1;
            const u32 sn = n - 2;
            section = kUnknown;
            if (word_is(s, sn, "machine"))
                section = kMachine;
            else if (word_is(s, sn, "floppy")) {
                section = kFloppy;
                r.floppy = true;
            }
            for (u32 i = 0; i < MachineConfig::kDrives; ++i)
                if (word_is(s, sn, MachineConfig::drive_name(i)))
                    section = kDrive0 + i;
            if (section == kUnknown)
                reject(r, line_no, "unknown section");
            continue;
        }

        u32 eq = 0;
        while (eq < n && p[eq] != '=')
            ++eq;
        if (eq == n) {
            reject(r, line_no, "expected key = value");
            continue;
        }
        u32 kn = eq;
        while (kn > 0 && is_space(p[kn - 1]))
            --kn;
        u32 vb = eq + 1;
        while (vb < n && is_space(p[vb]))
            ++vb;
        const char* key = p;
        const char* val = p + vb;
        const u32 vn = n - vb;

        if (section == kMachine) {
            u32 v = 0, w = 0, h = 0, d = 0;
            if (word_is(key, kn, "ram_mb")) {
                if (parse_u32(val, vn, v) && MachineConfig::valid_ram(v))
                    c.ram_mb = v;
                else
                    reject(r, line_no, "ram_mb: not one of the offered sizes");
            } else if (word_is(key, kn, "rom")) {
                if (vn == 0 || !copy(c.rom, MachineConfig::kPathMax, val, vn))
                    reject(r, line_no, "rom: empty or too long");
            } else if (word_is(key, kn, "boot")) {
                if (word_is(val, vn, "auto")) c.boot = MachineConfig::Boot::Auto;
                else if (word_is(val, vn, "cdrom")) c.boot = MachineConfig::Boot::CdRom;
                else if (word_is(val, vn, "disk")) c.boot = MachineConfig::Boot::Disk;
                else reject(r, line_no, "boot: auto, cdrom or disk");
            } else if (word_is(key, kn, "display")) {
                if (parse_mode(val, vn, w, h, d) && MachineConfig::valid_mode(w, h, d)) {
                    c.width = w;
                    c.height = h;
                    c.depth = d;
                } else {
                    reject(r, line_no, "display: not an offered mode");
                }
            } else if (word_is(key, kn, "jit")) {
                if (word_is(val, vn, "yes")) c.jit = true;
                else if (word_is(val, vn, "no")) c.jit = false;
                else reject(r, line_no, "jit: yes or no");
            } else if (word_is(key, kn, "web_password")) {
                if (!copy(c.web_password, MachineConfig::kPasswordMax, val, vn))
                    reject(r, line_no, "web_password: up to 63 characters");
            } else if (word_is(key, kn, "sound") || word_is(key, kn, "network") || word_is(key, kn, "web") ||
                       word_is(key, kn, "autostart")) {
                bool& flag = word_is(key, kn, "sound") ? c.sound : word_is(key, kn, "network") ? c.network
                             : word_is(key, kn, "web") ? c.web : c.autostart;
                if (word_is(val, vn, "yes")) flag = true;
                else if (word_is(val, vn, "no")) flag = false;
                else reject(r, line_no, "sound/network/web/autostart: yes or no");
            } else {
                reject(r, line_no, "unknown key in [machine]");
            }
        } else if (section >= kDrive0 && section < kFloppy) {
            MachineConfig::Drive& dr = c.drive[section - kDrive0];
            if (word_is(key, kn, "type")) {
                if (word_is(val, vn, "none")) dr.type = MachineConfig::DriveType::None;
                else if (word_is(val, vn, "disk")) dr.type = MachineConfig::DriveType::Disk;
                else if (word_is(val, vn, "cdrom")) dr.type = MachineConfig::DriveType::CdRom;
                else reject(r, line_no, "type: none, disk or cdrom");
                if (dr.type == MachineConfig::DriveType::CdRom)
                    dr.read_only = true;
            } else if (word_is(key, kn, "image")) {
                if (!copy(dr.image, MachineConfig::kPathMax, val, vn))
                    reject(r, line_no, "image: path too long");
            } else if (word_is(key, kn, "readonly")) {
                bool ro = false;
                if (parse_bool(val, vn, ro))
                    dr.read_only = ro || dr.type == MachineConfig::DriveType::CdRom;
                else
                    reject(r, line_no, "readonly: yes or no");
            } else {
                reject(r, line_no, "unknown key in a drive section");
            }
        } else if (section == kFloppy) {
            reject(r, line_no, "the Blue & White G3 has no floppy drive");
        } else if (section == kNone) {
            reject(r, line_no, "key outside a section");
        }
        // kUnknown: already reported at the section header.
    }
}

u32 write_machine_config(const MachineConfig& c, char* out, u32 cap)
{
    if (cap == 0)
        return 0;
    Out o{out, cap};
    o.str("; G3MacPi machine configuration (written by the emulator's menu).\n");
    o.str("; See docs/machine-config.md. Paths are on the SD card.\n\n");
    o.str("[machine]\nram_mb = ");
    o.num(c.ram_mb);
    o.str("\nrom = ");
    o.str(c.rom);
    o.str("\nboot = ");
    o.str(MachineConfig::boot_name(c.boot));
    o.str("\ndisplay = ");
    o.num(c.width);
    o.str("x");
    o.num(c.height);
    o.str("x");
    o.num(c.depth);
    o.str("\njit = ");
    o.str(c.jit ? "yes" : "no");
    o.str("\nsound = ");
    o.str(c.sound ? "yes" : "no");
    o.str("\nnetwork = ");
    o.str(c.network ? "yes" : "no");
    o.str("\nautostart = ");
    o.str(c.autostart ? "yes" : "no");
    o.str("\nweb = ");
    o.str(c.web ? "yes" : "no");
    if (c.web_password[0]) {
        o.str("\nweb_password = ");
        o.str(c.web_password);
    }
    o.str("\n");
    for (u32 i = 0; i < MachineConfig::kDrives; ++i) {
        const MachineConfig::Drive& d = c.drive[i];
        o.str("\n[");
        o.str(MachineConfig::drive_name(i));
        o.str("]\ntype = ");
        o.str(MachineConfig::type_name(d.type));
        o.str("\n");
        if (d.image[0]) {
            o.str("image = ");
            o.str(d.image);
            o.str("\n");
        }
        if (d.type == MachineConfig::DriveType::Disk) {
            o.str("readonly = ");
            o.str(d.read_only ? "yes" : "no");
            o.str("\n");
        }
    }
    if (!o.ok)
        return 0;
    out[o.len] = 0;
    return o.len;
}

u32 choose_boot_drive(const MachineConfig& c, const bool has_image[MachineConfig::kDrives])
{
    auto first = [&](MachineConfig::DriveType t) {
        for (u32 i = 0; i < MachineConfig::kDrives; ++i)
            if (c.drive[i].type == t && has_image[i])
                return i;
        return MachineConfig::kDrives;
    };
    using T = MachineConfig::DriveType;
    switch (c.boot) {
    case MachineConfig::Boot::CdRom: return first(T::CdRom);
    case MachineConfig::Boot::Disk: return first(T::Disk);
    case MachineConfig::Boot::Auto:
    default: {
        const u32 d = first(T::Disk);
        return d < MachineConfig::kDrives ? d : first(T::CdRom);
    }
    }
}

} // namespace g3
