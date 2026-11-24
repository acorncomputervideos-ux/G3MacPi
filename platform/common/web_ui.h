// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/web_ui.h — the web interface: one page (status, the
// settings file, the image folders with uploads, updating the emulator)
// and a small JSON API under /api, as an HttpHandler. Everything it does
// to files and the machine goes through WebHost, so the same code runs on
// the Pi (Circle, FatFs) and on Linux (tests, tools/webui_host).
//
//   GET  /                      the page
//   GET  /api/status            JSON: version, uptime, the Mac's state, addresses
//   GET  /api/config            the settings file (text)
//   PUT  /api/config            replace it; refused unless every line is valid
//   GET  /api/files?dir=D       JSON list of a folder (D: cd, hdd, rom, disks)
//   PUT  /api/upload?dir=D&name=N  the body becomes D/N (written as N.part,
//                               renamed when complete; refused while the Mac
//                               uses that file)
//   DELETE /api/files?dir=D&name=N
//   PUT  /api/update            a new emulator image (kernel8-rpi4.img): kept
//                               only if it is between 64 KB and 32 MB and
//                               contains kKernelMarker; the old image is kept
//                               as kernel8-rpi4.bak
//   POST /api/insert?drive=N[&dir=D&name=F]  put D/F into CD-ROM drive N
//                               (0-3), or eject it without dir/name; at once
//                               in a running Mac
//   POST /api/reboot            restart the Pi (after the response)
//
// With a password set (web_password in the settings), every request needs
// HTTP Basic authentication (RFC 7617) with that password (any user name).
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_WEB_UI_H
#define G3_PLATFORM_COMMON_WEB_UI_H

#include "core/common/types.h"
#include "platform/common/http_server.h"

namespace g3 {

// In every emulator image (app/circle/kernel.cpp keeps it): an upload
// without it is not a G3MacPi image and is refused.
#define G3_KERNEL_MARKER "G3MacPi-emulator-image-v1"

struct WebStatus {
    const char* version = "";
    u64 uptime_s = 0;
    const char* mac_state = "";   // e.g. "running", "settings menu", "stopped"
    u32 guest_mips_x10 = 0;       // emulation speed, tenths of MIPS
    char ip[20] = {};             // the Pi's address ("" if none yet)
    u8 host_mac[6] = {};
    u8 mac_mac[6] = {};
    bool sound = false, network = false;
    const char* port = "";        // the Pi's network connection, e.g. "Ethernet", "WiFi: Home (connected)"
    // The four drive positions: "none", "cdrom" or "disk", and the image.
    struct Drive { const char* label = ""; const char* type = "none"; const char* image = ""; };
    Drive drives[4];
};

struct WebFileEntry {
    char name[128];
    u64 size;
};

class WebHost {
public:
    virtual void status(WebStatus& out) = 0;
    // A whole small file (the settings); false if missing/unreadable.
    virtual bool read_file(const char* path, u8* buf, u32 cap, u32& len) = 0;
    virtual bool write_file(const char* path, const u8* data, u32 len) = 0;
    // Folder listing (files only); returns the number of entries.
    virtual u32 list(const char* dir, WebFileEntry* out, u32 max) = 0;
    // The running Mac has this image open.
    virtual bool in_use(const char* path) = 0;
    // Streaming writes (uploads).
    virtual void* open_write(const char* path) = 0;
    virtual bool write(void* file, const u8* data, u32 len) = 0;
    virtual bool close(void* file) = 0;  // false: the data did not all get written
    virtual bool remove(const char* path) = 0;
    virtual bool rename(const char* from, const char* to) = 0;
    virtual const char* password() = 0;  // "" for none
    virtual void reboot_after_response() = 0;
    virtual void config_changed() {}
    // Put a disc image into CD-ROM drive `drive` ("" ejects). 0: done,
    // 1: not a CD-ROM drive, 2: the image could not be opened.
    virtual u32 insert_media(u32 drive, const char* path) { (void)drive; (void)path; return 1; }

protected:
    ~WebHost() = default;
};

struct WebPaths {
    const char* config = "/g3macpi/g3macpi.ini";
    const char* kernel = "/kernel8-rpi4.img";
    const char* kernel_new = "/kernel8-rpi4.new";
    const char* kernel_old = "/kernel8-rpi4.bak";
    const char* cd = "/g3macpi/cd";
    const char* hdd = "/g3macpi/hdd";
    const char* rom = "/g3macpi/rom";
    const char* disks = "/g3macpi/disks";
};

class WebUi final : public HttpHandler {
public:
    static constexpr u32 kMaxConfig = 16384;
    static constexpr u64 kMinKernel = 64u << 10, kMaxKernel = 32u << 20;
    static constexpr u32 kMaxFiles = 128;

    WebUi(WebHost& host, const WebPaths& paths = WebPaths());

    // HttpHandler. A WebUi serves one connection at a time.
    bool begin(const HttpRequest& req, HttpResponse& resp) override;
    bool body(const u8* data, u32 len) override;
    void finish(bool complete, HttpResponse& resp) override;

    // A valid name for an uploaded file: 1-100 characters, no '/', '\\',
    // ':' or control characters, not "." or "..", not ending in ".part".
    static bool valid_name(const char* name);

private:
    enum class Body : u8 { None, Config, Upload, Update };
    const char* folder(const HttpRequest& req) const;
    bool authorized(const HttpRequest& req) const;
    void text(HttpResponse& r, u32 status, const char* msg);
    void json_status(HttpResponse& r);
    void json_files(HttpResponse& r, const char* dir);
    void marker_scan(const u8* data, u32 len);

    WebHost& host_;
    WebPaths paths_;
    Body body_ = Body::None;
    void* file_ = nullptr;
    bool write_ok_ = true;
    u64 received_ = 0;
    char target_[384] = {};
    char part_[400] = {};
    u32 marker_match_ = 0;   // bytes of the marker matched so far
    bool marker_found_ = false;
    u8 config_[kMaxConfig + 1] = {};
    u32 config_len_ = 0;
    char out_[24576] = {};   // response text
    WebFileEntry files_[kMaxFiles] = {};
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_WEB_UI_H
