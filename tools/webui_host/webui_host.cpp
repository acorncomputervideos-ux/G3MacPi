// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/webui_host/webui_host.cpp — the Pi's web interface on Linux, for
// trying and testing it: g3_webui_host <root folder> [port] [password]
// serves the same WebUi as the Pi, with the SD card's paths inside <root>
// (e.g. <root>/g3macpi/cd). One connection at a time, like the Pi.

#include "platform/common/http_server.h"
#include "platform/common/net_switch.h"
#include "platform/common/web_ui.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

using namespace g3;

namespace {

struct FsHost final : WebHost {
    std::string root, pw;
    time_t start = time(nullptr);
    bool reboot = false;
    std::string at(const char* p) const { return root + p; }

    void status(WebStatus& s) override
    {
        s.version = "host";
        s.uptime_s = static_cast<u64>(time(nullptr) - start);
        s.mac_state = "not running (Linux test server)";
        std::strcpy(s.ip, "127.0.0.1");
        const u8 m[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        std::memcpy(s.host_mac, m, 6);
        net_switch::mac_address_for(m, s.mac_mac);
        s.drives[0] = {"CD-ROM bus master", "cdrom", cd.c_str()};
        s.drives[2] = {"Hard disk bus master", "none", ""};
    }
    bool read_file(const char* p, u8* buf, u32 cap, u32& len) override
    {
        FILE* f = std::fopen(at(p).c_str(), "rb");
        if (!f)
            return false;
        len = static_cast<u32>(std::fread(buf, 1, cap, f));
        std::fclose(f);
        return true;
    }
    bool write_file(const char* p, const u8* d, u32 n) override
    {
        FILE* f = std::fopen(at(p).c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(d, 1, n, f) == n;
        return std::fclose(f) == 0 && ok;
    }
    u32 list(const char* dir, WebFileEntry* out, u32 max) override
    {
        DIR* d = opendir(at(dir).c_str());
        u32 n = 0;
        if (!d)
            return 0;
        while (dirent* e = readdir(d)) {
            struct stat st;
            const std::string full = at(dir) + "/" + e->d_name;
            if (n >= max || stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            std::snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
            out[n++].size = static_cast<u64>(st.st_size);
        }
        closedir(d);
        return n;
    }
    bool in_use(const char*) override { return false; }
    void* open_write(const char* p) override { return std::fopen(at(p).c_str(), "wb"); }
    bool write(void* f, const u8* d, u32 n) override { return std::fwrite(d, 1, n, static_cast<FILE*>(f)) == n; }
    bool close(void* f) override { return std::fclose(static_cast<FILE*>(f)) == 0; }
    bool remove(const char* p) override { return ::remove(at(p).c_str()) == 0; }
    bool rename(const char* a, const char* b) override { return ::rename(at(a).c_str(), at(b).c_str()) == 0; }
    const char* password() override { return pw.c_str(); }
    void reboot_after_response() override { reboot = true; }
    // One CD-ROM drive (drive 0) for trying inserts and ejects.
    std::string cd;
    u32 insert_media(u32 drive, const char* p) override
    {
        if (drive != 0)
            return 1;
        if (p[0] && access(at(p).c_str(), R_OK) != 0)
            return 2;
        cd = p;
        return 0;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: g3_webui_host <root folder> [port] [password]\n");
        return 2;
    }
    FsHost host;
    host.root = argv[1];
    const int port = argc > 2 ? std::atoi(argv[2]) : 8080;
    if (argc > 3)
        host.pw = argv[3];
    for (const char* d : {"/g3macpi", "/g3macpi/cd", "/g3macpi/hdd", "/g3macpi/rom", "/g3macpi/disks"})
        mkdir(host.at(d).c_str(), 0755);
    WebUi ui(host);

    const int s = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u16>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0) {
        std::perror("listen");
        return 1;
    }
    std::printf("[webui] http://127.0.0.1:%d/ serving %s\n", port, argv[1]);
    std::fflush(stdout);
    static u8 buf[65536];
    for (;;) {
        const int c = accept(s, nullptr, nullptr);
        if (c < 0)
            continue;
        HttpConnection conn(ui, [](void* ctx, const u8* d, u32 n) {
            const int fd = *static_cast<int*>(ctx);
            while (n) {
                const ssize_t k = send(fd, d, n, MSG_NOSIGNAL);
                if (k <= 0)
                    return false;
                d += k;
                n -= static_cast<u32>(k);
            }
            return true;
        }, const_cast<int*>(&c));
        for (;;) {
            const ssize_t n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) {
                conn.closed();
                break;
            }
            if (!conn.feed(buf, static_cast<u32>(n)))
                break;
        }
        ::close(c);
        if (host.reboot) {
            std::printf("[webui] restart requested\n");
            std::fflush(stdout);
            host.reboot = false;
        }
    }
}
