// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/web_ui.cpp — see web_ui.h.

#include "platform/common/web_ui.h"

#include "core/common/version.h"
#include "core/machine/machine_config.h"

namespace g3 {

namespace {

const char kPage[] =
#include "platform/common/web_page.inc"
    ;

const char kMarker[] = G3_KERNEL_MARKER;

u32 slen(const char* s)
{
    u32 n = 0;
    while (s[n])
        ++n;
    return n;
}

bool seq(const char* a, const char* b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

bool starts(const char* s, const char* prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return false;
    return true;
}

struct Out {
    char* p;
    u32 cap, n = 0;
    void ch(char c)
    {
        if (n + 1 < cap) {
            p[n++] = c;
            p[n] = 0;
        }
    }
    void str(const char* s)
    {
        while (*s)
            ch(*s++);
    }
    void num(u64 v)
    {
        char t[24];
        u32 k = 0;
        do {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k)
            ch(t[--k]);
    }
    void hex2(u8 v)
    {
        const char* h = "0123456789abcdef";
        ch(h[v >> 4]);
        ch(h[v & 15]);
    }
    // A JSON string (RFC 8259 escaping).
    void jstr(const char* s)
    {
        ch('"');
        for (; *s; ++s) {
            const u8 c = static_cast<u8>(*s);
            if (c == '"' || c == '\\') {
                ch('\\');
                ch(static_cast<char>(c));
            } else if (c < 0x20) {
                str("\\u00");
                hex2(c);
            } else {
                ch(static_cast<char>(c));
            }
        }
        ch('"');
    }
    void mac(const u8* m)
    {
        ch('"');
        for (u32 i = 0; i < 6; ++i) {
            if (i)
                ch(':');
            hex2(m[i]);
        }
        ch('"');
    }
};

void join(char* out, u32 cap, const char* dir, const char* name, const char* suffix = "")
{
    Out o{out, cap};
    o.str(dir);
    o.ch('/');
    o.str(name);
    o.str(suffix);
}

} // namespace

WebUi::WebUi(WebHost& host, const WebPaths& paths) : host_(host), paths_(paths) {}

bool WebUi::valid_name(const char* name)
{
    const u32 n = slen(name);
    if (n == 0 || n > 100 || seq(name, ".") || seq(name, ".."))
        return false;
    for (u32 i = 0; i < n; ++i) {
        const u8 c = static_cast<u8>(name[i]);
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':')
            return false;
    }
    return !(n >= 5 && seq(name + n - 5, ".part"));
}

const char* WebUi::folder(const HttpRequest& req) const
{
    char d[16];
    if (!req.param("dir", d, sizeof d))
        return nullptr;
    if (seq(d, "cd")) return paths_.cd;
    if (seq(d, "hdd")) return paths_.hdd;
    if (seq(d, "rom")) return paths_.rom;
    if (seq(d, "disks")) return paths_.disks;
    return nullptr;
}

bool WebUi::authorized(const HttpRequest& req) const
{
    const char* pw = host_.password();
    if (!pw || !pw[0])
        return true;
    if (!starts(req.authorization, "Basic "))
        return false;
    u8 dec[192];
    u32 n = 0;
    if (!http::base64_decode(req.authorization + 6, dec, sizeof dec - 1, &n))
        return false;
    dec[n] = 0;
    // "user:password": any user name.
    u32 colon = 0;
    while (colon < n && dec[colon] != ':')
        ++colon;
    return colon < n && seq(reinterpret_cast<const char*>(dec + colon + 1), pw);
}

void WebUi::text(HttpResponse& r, u32 status, const char* msg)
{
    Out o{out_, sizeof out_};
    o.str(msg);
    o.ch('\n');
    r.set(status, "text/plain; charset=utf-8", out_);
}

void WebUi::json_status(HttpResponse& r)
{
    WebStatus s;
    host_.status(s);
    Out o{out_, sizeof out_};
    o.str("{\"name\":");
    o.jstr(G3_PROJECT_NAME);
    o.str(",\"version\":");
    o.jstr(s.version[0] ? s.version : G3_VERSION_STRING);
    o.str(",\"uptime_s\":");
    o.num(s.uptime_s);
    o.str(",\"mac_state\":");
    o.jstr(s.mac_state);
    o.str(",\"mips\":\"");
    o.num(s.guest_mips_x10 / 10);
    o.ch('.');
    o.num(s.guest_mips_x10 % 10);
    o.str("\",\"ip\":");
    o.jstr(s.ip);
    o.str(",\"pi_ethernet\":");
    o.mac(s.host_mac);
    o.str(",\"mac_ethernet\":");
    o.mac(s.mac_mac);
    o.str(",\"sound\":");
    o.str(s.sound ? "true" : "false");
    o.str(",\"network\":");
    o.str(s.network ? "true" : "false");
    o.str(",\"port\":");
    o.jstr(s.port);
    o.str(",\"drives\":[");
    for (u32 i = 0; i < 4; ++i) {
        if (i)
            o.ch(',');
        o.str("{\"label\":");
        o.jstr(s.drives[i].label);
        o.str(",\"type\":");
        o.jstr(s.drives[i].type);
        o.str(",\"image\":");
        o.jstr(s.drives[i].image);
        o.ch('}');
    }
    o.str("],\"password\":");
    o.str(host_.password()[0] ? "true" : "false");
    o.str("}");
    r.set(200, "application/json", out_);
}

void WebUi::json_files(HttpResponse& r, const char* dir)
{
    const u32 n = host_.list(dir, files_, kMaxFiles);
    Out o{out_, sizeof out_};
    o.str("{\"dir\":");
    o.jstr(dir);
    o.str(",\"files\":[");
    u32 shown = 0;
    for (u32 i = 0; i < n; ++i) {
        if (!valid_name(files_[i].name))
            continue;  // e.g. an unfinished .part
        if (shown++)
            o.ch(',');
        o.str("{\"name\":");
        o.jstr(files_[i].name);
        o.str(",\"size\":");
        o.num(files_[i].size);
        char path[384];
        join(path, sizeof path, dir, files_[i].name);
        o.str(",\"in_use\":");
        o.str(host_.in_use(path) ? "true" : "false");
        o.ch('}');
    }
    o.str("]}");
    r.set(200, "application/json", out_);
}

bool WebUi::begin(const HttpRequest& req, HttpResponse& r)
{
    using M = HttpRequest::Method;
    body_ = Body::None;
    file_ = nullptr;
    write_ok_ = true;
    received_ = 0;
    if (!authorized(req)) {
        text(r, 401, "A password is needed (web_password in the settings).");
        r.extra_header = "WWW-Authenticate: Basic realm=\"G3MacPi\", charset=\"UTF-8\"";
        return false;
    }
    const bool get = req.method == M::Get || req.method == M::Head;
    if (seq(req.path, "/") || seq(req.path, "/index.html")) {
        if (!get) {
            text(r, 405, "Method not allowed.");
            return false;
        }
        r.set(200, "text/html; charset=utf-8", kPage);
        return false;
    }
    if (seq(req.path, "/api/status") && get) {
        json_status(r);
        return false;
    }
    if (seq(req.path, "/api/config")) {
        if (get) {
            u32 n = 0;
            if (!host_.read_file(paths_.config, config_, kMaxConfig, n))
                n = 0;  // no file yet: the defaults apply
            config_[n] = 0;
            r.status = 200;
            r.content_type = "text/plain; charset=utf-8";
            r.body = config_;
            r.body_len = n;
            return false;
        }
        if (req.method == M::Put) {
            if (!req.has_length || req.content_length > kMaxConfig) {
                text(r, 413, "The settings file is limited to 16 KB.");
                return false;
            }
            body_ = Body::Config;
            config_len_ = 0;
            return true;
        }
    }
    if (seq(req.path, "/api/files")) {
        const char* dir = folder(req);
        if (!dir) {
            text(r, 404, "Unknown folder (cd, hdd, rom or disks).");
            return false;
        }
        if (get) {
            json_files(r, dir);
            return false;
        }
        if (req.method == M::Delete) {
            char name[128];
            if (!req.param("name", name, sizeof name) || !valid_name(name)) {
                text(r, 400, "Bad file name.");
                return false;
            }
            join(target_, sizeof target_, dir, name);
            if (host_.in_use(target_))
                text(r, 409, "The Mac is using that file.");
            else if (!host_.remove(target_))
                text(r, 404, "Could not delete it.");
            else
                text(r, 200, "Deleted.");
            return false;
        }
    }
    if (seq(req.path, "/api/upload") && req.method == M::Put) {
        const char* dir = folder(req);
        char name[128];
        if (!dir) {
            text(r, 404, "Unknown folder (cd, hdd, rom or disks).");
            return false;
        }
        if (!req.param("name", name, sizeof name) || !valid_name(name)) {
            text(r, 400, "Bad file name (1-100 characters, no / \\ : and not ending in .part).");
            return false;
        }
        if (!req.has_length) {
            text(r, 411, "The upload needs a Content-Length.");
            return false;
        }
        join(target_, sizeof target_, dir, name);
        if (host_.in_use(target_)) {
            text(r, 409, "The Mac is using that file: eject it or stop the Mac first.");
            return false;
        }
        join(part_, sizeof part_, dir, name, ".part");
        file_ = host_.open_write(part_);
        if (!file_) {
            text(r, 500, "Could not create the file on the SD card.");
            return false;
        }
        body_ = Body::Upload;
        return true;
    }
    if (seq(req.path, "/api/update") && req.method == M::Put) {
        if (!req.has_length || req.content_length < kMinKernel || req.content_length > kMaxKernel) {
            text(r, 413, "Not an emulator image: kernel8-rpi4.img is between 64 KB and 32 MB.");
            return false;
        }
        file_ = host_.open_write(paths_.kernel_new);
        if (!file_) {
            text(r, 500, "Could not create the file on the SD card.");
            return false;
        }
        marker_match_ = 0;
        marker_found_ = false;
        body_ = Body::Update;
        return true;
    }
    if (seq(req.path, "/api/insert") && req.method == M::Post) {
        char num[8], name[128];
        if (!req.param("drive", num, sizeof num) || num[0] < '0' || num[0] > '3' || num[1]) {
            text(r, 400, "Which drive (0-3)?");
            return false;
        }
        target_[0] = 0;
        if (req.param("name", name, sizeof name)) {
            const char* dir = folder(req);
            if (!dir || !valid_name(name)) {
                text(r, 400, "Bad folder or file name.");
                return false;
            }
            join(target_, sizeof target_, dir, name);
        }
        const u32 result = host_.insert_media(static_cast<u32>(num[0] - '0'), target_);
        if (result == 1)
            text(r, 409, "That drive is not a CD-ROM drive.");
        else if (result == 2)
            text(r, 404, "Could not open that image.");
        else
            text(r, 200, target_[0] ? "Inserted." : "Ejected.");
        return false;
    }
    if (seq(req.path, "/api/reboot") && req.method == M::Post) {
        host_.reboot_after_response();
        text(r, 200, "Restarting.");
        return false;
    }
    if (starts(req.path, "/api/")) {
        text(r, 405, "Not available.");
        return false;
    }
    text(r, 404, "Not found.");
    return false;
}

void WebUi::marker_scan(const u8* data, u32 len)
{
    const u32 m = sizeof kMarker - 1;
    for (u32 i = 0; i < len && !marker_found_; ++i) {
        // The marker's first character does not occur again in it, so a
        // mismatch can only restart at the current byte.
        if (data[i] == static_cast<u8>(kMarker[marker_match_]))
            ++marker_match_;
        else
            marker_match_ = data[i] == static_cast<u8>(kMarker[0]) ? 1 : 0;
        if (marker_match_ == m)
            marker_found_ = true;
    }
}

bool WebUi::body(const u8* data, u32 len)
{
    received_ += len;
    switch (body_) {
    case Body::Config:
        for (u32 i = 0; i < len && config_len_ < kMaxConfig; ++i)
            config_[config_len_++] = data[i];
        return true;
    case Body::Update:
        marker_scan(data, len);
        [[fallthrough]];
    case Body::Upload:
        if (write_ok_)
            write_ok_ = host_.write(file_, data, len);
        return write_ok_;
    default:
        return false;
    }
}

void WebUi::finish(bool complete, HttpResponse& r)
{
    switch (body_) {
    case Body::Config: {
        if (!complete) {
            text(r, 400, "The settings did not all arrive.");
            break;
        }
        config_[config_len_] = 0;
        MachineConfig c;
        ConfigParseReport rep;
        parse_machine_config(reinterpret_cast<const char*>(config_), config_len_, c, rep);
        if (rep.ignored) {
            Out o{out_, sizeof out_};
            o.str("Not saved: line ");
            o.num(rep.first_line);
            o.str(": ");
            o.str(rep.first_reason);
            o.str(rep.ignored > 1 ? " (and more)\n" : "\n");
            r.set(400, "text/plain; charset=utf-8", out_);
            break;
        }
        if (!host_.write_file(paths_.config, config_, config_len_)) {
            text(r, 500, "Could not write the settings file.");
            break;
        }
        host_.config_changed();
        text(r, 200, "Saved. Changes apply when the Mac next starts.");
        break;
    }
    case Body::Upload: {
        const bool closed = host_.close(file_);
        file_ = nullptr;
        if (!complete || !write_ok_ || !closed) {
            host_.remove(part_);
            text(r, write_ok_ ? 400 : 507, write_ok_ ? "The upload did not finish; nothing was kept."
                                                     : "Writing to the SD card failed (full?); nothing was kept.");
            break;
        }
        host_.remove(target_);  // replacing an older file of that name
        if (!host_.rename(part_, target_)) {
            host_.remove(part_);
            text(r, 500, "Could not put the file in place.");
            break;
        }
        Out o{out_, sizeof out_};
        o.str("Uploaded ");
        o.num(received_);
        o.str(" bytes to ");
        o.str(target_);
        o.ch('\n');
        r.set(200, "text/plain; charset=utf-8", out_);
        break;
    }
    case Body::Update: {
        const bool closed = host_.close(file_);
        file_ = nullptr;
        if (!complete || !write_ok_ || !closed) {
            host_.remove(paths_.kernel_new);
            text(r, write_ok_ ? 400 : 507, "The image did not arrive in full; the emulator was not changed.");
            break;
        }
        if (!marker_found_) {
            host_.remove(paths_.kernel_new);
            text(r, 400, "That file is not a G3MacPi emulator image (kernel8-rpi4.img); nothing was changed.");
            break;
        }
        // Keep the running image as the backup, then put the new one in place.
        host_.remove(paths_.kernel_old);
        if (!host_.rename(paths_.kernel, paths_.kernel_old) || !host_.rename(paths_.kernel_new, paths_.kernel)) {
            // Put things back as far as possible.
            host_.rename(paths_.kernel_old, paths_.kernel);
            text(r, 500, "Could not replace the image; the old one stays.");
            break;
        }
        text(r, 200, "Updated. Restart the Pi to run the new emulator (the old one is kept as kernel8-rpi4.bak).");
        break;
    }
    default:
        text(r, 400, "Bad request.");
        break;
    }
    body_ = Body::None;
}

} // namespace g3
