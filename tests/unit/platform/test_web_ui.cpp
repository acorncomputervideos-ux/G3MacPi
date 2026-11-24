// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_web_ui.cpp — the web interface (HTTP connection
// + WebUi) against an in-memory host: requests arrive in random pieces, as
// TCP delivers them; uploads must land complete or not at all; the settings
// are checked before saving; an emulator update is only installed when it
// is one. Also the Ethernet switch's routing.

#include "platform/common/http_server.h"
#include "platform/common/net_switch.h"
#include "platform/common/web_ui.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace g3;

namespace {

struct MemHost final : WebHost {
    std::map<std::string, std::vector<u8>> files;
    std::set<std::string> busy;
    std::string pw;
    bool rebooted = false;
    int config_changes = 0;
    u64 fail_after = ~0ull;  // writes fail once this many bytes were written
    u64 written = 0;
    struct Open { std::string path; std::vector<u8> data; };
    std::vector<std::unique_ptr<Open>> open;

    void status(WebStatus& s) override
    {
        s.version = "test";
        s.uptime_s = 3725;
        s.mac_state = "running";
        s.guest_mips_x10 = 123;
        std::strcpy(s.ip, "192.168.1.20");
        const u8 m[6] = {0xdc, 0xa6, 0x32, 1, 2, 3};
        std::memcpy(s.host_mac, m, 6);
        net_switch::mac_address_for(m, s.mac_mac);
        s.sound = true;
    }
    bool read_file(const char* p, u8* buf, u32 cap, u32& len) override
    {
        auto it = files.find(p);
        if (it == files.end() || it->second.size() > cap)
            return false;
        std::memcpy(buf, it->second.data(), it->second.size());
        len = static_cast<u32>(it->second.size());
        return true;
    }
    bool write_file(const char* p, const u8* d, u32 n) override
    {
        files[p].assign(d, d + n);
        return true;
    }
    u32 list(const char* dir, WebFileEntry* out, u32 max) override
    {
        const std::string pre = std::string(dir) + "/";
        u32 n = 0;
        for (auto& [k, v] : files)
            if (k.compare(0, pre.size(), pre) == 0 && k.find('/', pre.size()) == std::string::npos && n < max) {
                std::strncpy(out[n].name, k.c_str() + pre.size(), sizeof out[n].name - 1);
                out[n].name[sizeof out[n].name - 1] = 0;
                out[n++].size = v.size();
            }
        return n;
    }
    bool in_use(const char* p) override { return busy.count(p) != 0; }
    void* open_write(const char* p) override
    {
        open.push_back(std::make_unique<Open>(Open{p, {}}));
        files[p];  // exists from now on, as on a real disk
        return open.back().get();
    }
    bool write(void* f, const u8* d, u32 n) override
    {
        if (written + n > fail_after)
            return false;
        written += n;
        auto* o = static_cast<Open*>(f);
        o->data.insert(o->data.end(), d, d + n);
        return true;
    }
    bool close(void* f) override
    {
        auto* o = static_cast<Open*>(f);
        files[o->path] = o->data;
        return true;
    }
    bool remove(const char* p) override { return files.erase(p) != 0; }
    bool rename(const char* a, const char* b) override
    {
        auto it = files.find(a);
        if (it == files.end() || files.count(b))
            return false;
        files[b] = it->second;
        files.erase(a);
        return true;
    }
    const char* password() override { return pw.c_str(); }
    void reboot_after_response() override { rebooted = true; }
    void config_changed() override { ++config_changes; }
};

struct Reply {
    u32 status = 0;
    std::string head, body;
};

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 7) {}
    u32 next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u32>(s >> 11);
    }
};

// One request through a fresh connection, fed in random pieces. stop_at:
// the client goes away after that many bytes.
Reply exchange(WebUi& ui, const std::string& request, u32 seed = 1, size_t stop_at = std::string::npos)
{
    std::string out;
    HttpConnection c(ui, [](void* ctx, const u8* d, u32 n) {
        static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(d), n);
        return true;
    }, &out);
    Rng g(seed);
    size_t pos = 0;
    const size_t end = stop_at < request.size() ? stop_at : request.size();
    while (pos < end) {
        size_t n = 1 + g.next() % 3000;
        if (n > end - pos)
            n = end - pos;
        if (!c.feed(reinterpret_cast<const u8*>(request.data() + pos), static_cast<u32>(n)))
            break;
        pos += n;
    }
    if (!c.finished())
        c.closed();
    Reply r;
    const size_t sep = out.find("\r\n\r\n");
    if (sep == std::string::npos)
        return r;
    r.head = out.substr(0, sep);
    r.body = out.substr(sep + 4);
    r.status = static_cast<u32>(std::atoi(out.c_str() + 9));
    return r;
}

std::string req(const std::string& method, const std::string& target, const std::string& body = "",
                const std::string& extra = "")
{
    std::string r = method + " " + target + " HTTP/1.1\r\nHost: pi\r\n" + extra;
    if (method == "PUT" || method == "POST")
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    return r + "\r\n" + body;
}

std::string blob(size_t n, u32 seed)
{
    std::string s(n, 0);
    Rng g(seed);
    for (auto& c : s)
        c = static_cast<char>(g.next());
    return s;
}

std::string str(const std::vector<u8>& v) { return std::string(v.begin(), v.end()); }

} // namespace

TEST_CASE("platform/web/page_status_and_errors")
{
    MemHost h;
    WebUi ui(h);
    Reply r = exchange(ui, req("GET", "/"));
    CHECK_EQ(r.status, 200u);
    CHECK(r.head.find("text/html") != std::string::npos);
    CHECK(r.body.find("<title>G3MacPi</title>") != std::string::npos);
    CHECK(r.head.find("Content-Length: " + std::to_string(r.body.size())) != std::string::npos);
    r = exchange(ui, req("HEAD", "/"));
    CHECK_EQ(r.status, 200u);
    CHECK(r.body.empty());
    r = exchange(ui, req("GET", "/api/status"));
    CHECK_EQ(r.status, 200u);
    CHECK(r.body.find("\"mips\":\"12.3\"") != std::string::npos);
    CHECK(r.body.find("\"ip\":\"192.168.1.20\"") != std::string::npos);
    CHECK(r.body.find("\"pi_ethernet\":\"dc:a6:32:01:02:03\"") != std::string::npos);
    CHECK(r.body.find("\"mac_ethernet\":\"de:a6:32:01:02:03\"") != std::string::npos);
    CHECK_EQ(exchange(ui, req("GET", "/nothing")).status, 404u);
    CHECK_EQ(exchange(ui, req("POST", "/")).status, 405u);
    CHECK_EQ(exchange(ui, "GARBAGE\r\n\r\n").status, 400u);
    CHECK_EQ(exchange(ui, "GET / HTTP/1.1\r\nX: " + std::string(9000, 'a') + "\r\n\r\n").status, 431u);
    CHECK_EQ(exchange(ui, "PUT /api/config HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").status, 411u);
    CHECK(exchange(ui, req("POST", "/api/reboot")).status == 200u);
    CHECK(h.rebooted);
}

TEST_CASE("platform/web/password")
{
    MemHost h;
    h.pw = "s3cret";
    WebUi ui(h);
    Reply r = exchange(ui, req("GET", "/"));
    CHECK_EQ(r.status, 401u);
    CHECK(r.head.find("WWW-Authenticate: Basic realm=\"G3MacPi\"") != std::string::npos);
    // "mac:s3cret" and "mac:wrong" in base64 (RFC 4648).
    CHECK_EQ(exchange(ui, req("GET", "/", "", "Authorization: Basic bWFjOnMzY3JldA==\r\n")).status, 200u);
    CHECK_EQ(exchange(ui, req("GET", "/", "", "Authorization: Basic bWFjOndyb25n\r\n")).status, 401u);
    CHECK_EQ(exchange(ui, req("GET", "/", "", "Authorization: Basic !!!\r\n")).status, 401u);
    // Even uploads are refused before anything is written.
    CHECK_EQ(exchange(ui, req("PUT", "/api/upload?dir=cd&name=a.iso", "data")).status, 401u);
    CHECK(h.files.empty());
}

TEST_CASE("platform/web/settings_checked_before_saving")
{
    MemHost h;
    WebUi ui(h);
    const WebPaths p;
    CHECK_EQ(exchange(ui, req("GET", "/api/config")).status, 200u);  // no file yet: empty
    const std::string good = "[machine]\nram_mb = 128\nsound = no\n";
    Reply r = exchange(ui, req("PUT", "/api/config", good));
    CHECK_EQ(r.status, 200u);
    CHECK(str(h.files[p.config]) == good);
    CHECK_EQ(h.config_changes, 1);
    CHECK(exchange(ui, req("GET", "/api/config")).body == good);
    r = exchange(ui, req("PUT", "/api/config", "[machine]\nram_mb = 3\n"));
    CHECK_EQ(r.status, 400u);
    CHECK(r.body.find("line 2") != std::string::npos);
    CHECK(str(h.files[p.config]) == good);  // unchanged
    CHECK_EQ(exchange(ui, req("PUT", "/api/config", std::string(20000, ';'))).status, 413u);
    CHECK_EQ(h.config_changes, 1);
}

TEST_CASE("platform/web/uploads_complete_or_not_at_all")
{
    MemHost h;
    WebUi ui(h);
    // 3 MB in random pieces, a name with a space and a plus.
    const std::string data = blob(3u << 20, 5);
    Reply r = exchange(ui, req("PUT", "/api/upload?dir=cd&name=Mac%20OS+9.iso", data), 9);
    CHECK_EQ(r.status, 200u);
    REQUIRE(h.files.count("/g3macpi/cd/Mac OS 9.iso"));
    CHECK(str(h.files["/g3macpi/cd/Mac OS 9.iso"]) == data);
    CHECK(!h.files.count("/g3macpi/cd/Mac OS 9.iso.part"));
    // Listed (an unfinished .part is not).
    h.files["/g3macpi/cd/x.iso.part"] = {1, 2};
    r = exchange(ui, req("GET", "/api/files?dir=cd"));
    CHECK(r.body.find("{\"name\":\"Mac OS 9.iso\",\"size\":3145728,\"in_use\":false}") != std::string::npos);
    CHECK(r.body.find("x.iso.part") == std::string::npos);
    h.files.erase("/g3macpi/cd/x.iso.part");
    // Replacing a file.
    const std::string data2 = blob(1000, 6);
    CHECK_EQ(exchange(ui, req("PUT", "/api/upload?dir=cd&name=Mac%20OS%209.iso", data2), 3).status, 200u);
    CHECK(str(h.files["/g3macpi/cd/Mac OS 9.iso"]) == data2);
    // In use by the Mac: refused, untouched.
    h.busy.insert("/g3macpi/hdd/sys.img");
    h.files["/g3macpi/hdd/sys.img"] = {9, 9, 9};
    CHECK_EQ(exchange(ui, req("PUT", "/api/upload?dir=hdd&name=sys.img", "new")).status, 409u);
    CHECK_EQ(exchange(ui, req("DELETE", "/api/files?dir=hdd&name=sys.img")).status, 409u);
    CHECK_EQ(h.files["/g3macpi/hdd/sys.img"].size(), 3u);
    // Bad names and folders.
    for (const char* bad : {"..", "a%2Fb", "x.part", "", "a%5Cb", "c%3Ad"})
        CHECK_EQ(exchange(ui, req("PUT", std::string("/api/upload?dir=cd&name=") + bad, "x")).status, 400u);
    CHECK_EQ(exchange(ui, req("PUT", "/api/upload?dir=etc&name=a", "x")).status, 404u);
    CHECK_EQ(exchange(ui, req("PUT", "/api/upload?dir=cd", "x")).status, 400u);
    // The client goes away half-way: nothing kept.
    const std::string big = req("PUT", "/api/upload?dir=rom&name=Mac%20OS%20ROM", blob(500000, 7));
    exchange(ui, big, 4, big.size() / 2);
    CHECK(!h.files.count("/g3macpi/rom/Mac OS ROM"));
    CHECK(!h.files.count("/g3macpi/rom/Mac OS ROM.part"));
    // The card fills up: 507, nothing kept.
    h.fail_after = h.written + 100000;
    r = exchange(ui, req("PUT", "/api/upload?dir=hdd&name=big.img", blob(300000, 8)));
    CHECK_EQ(r.status, 507u);
    CHECK(!h.files.count("/g3macpi/hdd/big.img"));
    CHECK(!h.files.count("/g3macpi/hdd/big.img.part"));
    h.fail_after = ~0ull;
    // Delete.
    CHECK_EQ(exchange(ui, req("DELETE", "/api/files?dir=cd&name=Mac%20OS%209.iso")).status, 200u);
    CHECK(!h.files.count("/g3macpi/cd/Mac OS 9.iso"));
}

TEST_CASE("platform/web/emulator_update_only_when_it_is_one")
{
    MemHost h;
    WebUi ui(h);
    const WebPaths p;
    h.files[p.kernel] = {1, 2, 3};
    // A real image contains the marker; put it across piece boundaries.
    std::string img = blob(2u << 20, 11);
    const std::string marker = G3_KERNEL_MARKER;
    img.replace(1234567, marker.size(), marker);
    for (u32 seed = 1; seed <= 3; ++seed) {
        h.files[p.kernel] = {1, 2, 3};
        h.files.erase(p.kernel_old);
        Reply r = exchange(ui, req("PUT", "/api/update", img), seed);
        CHECK_EQ(r.status, 200u);
        CHECK(str(h.files[p.kernel]) == img);
        CHECK_EQ(h.files[p.kernel_old].size(), 3u);  // the old one kept
        CHECK(!h.files.count(p.kernel_new));
    }
    // Not an emulator image: refused, nothing changed.
    h.files[p.kernel] = {1, 2, 3};
    std::string other = blob(1u << 20, 12);
    other.replace(5000, 10, marker.substr(0, 10));  // a partial marker is not enough
    Reply r = exchange(ui, req("PUT", "/api/update", other));
    CHECK_EQ(r.status, 400u);
    CHECK_EQ(h.files[p.kernel].size(), 3u);
    CHECK(!h.files.count(p.kernel_new));
    // Too small, too big, cut off.
    CHECK_EQ(exchange(ui, req("PUT", "/api/update", marker)).status, 413u);
    CHECK_EQ(exchange(ui, "PUT /api/update HTTP/1.1\r\nContent-Length: 40000000\r\n\r\n").status, 413u);
    const std::string cut = req("PUT", "/api/update", img);
    exchange(ui, cut, 2, cut.size() - 10);
    CHECK_EQ(h.files[p.kernel].size(), 3u);
    CHECK(!h.files.count(p.kernel_new));
}

TEST_CASE("platform/net_switch/routes_and_addresses")
{
    const u8 host[6] = {0xdc, 0xa6, 0x32, 0x11, 0x22, 0x33};
    u8 mac[6];
    net_switch::mac_address_for(host, mac);
    CHECK_EQ(mac[0], 0xdeu);      // locally administered
    CHECK_EQ(mac[0] & 1, 0u);     // unicast
    CHECK(std::memcmp(mac, host, 6) != 0);
    const u8 local[6] = {0x02, 0, 0, 0, 0, 5};
    u8 mac2[6];
    net_switch::mac_address_for(local, mac2);  // already local: differs in the last byte
    CHECK(std::memcmp(mac2, local, 6) != 0);
    u8 f[64] = {};
    std::memcpy(f, mac, 6);
    CHECK_EQ(net_switch::route(f, 64, mac, host), u32(net_switch::kToMac));
    std::memcpy(f, host, 6);
    CHECK_EQ(net_switch::route(f, 64, mac, host), u32(net_switch::kToHost));
    std::memset(f, 0xFF, 6);
    CHECK_EQ(net_switch::route(f, 64, mac, host), u32(net_switch::kToMac | net_switch::kToHost));
    const u8 at[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};  // AppleTalk broadcast
    std::memcpy(f, at, 6);
    CHECK_EQ(net_switch::route(f, 64, mac, host), u32(net_switch::kToMac | net_switch::kToHost));
    const u8 other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(f, other, 6);
    CHECK_EQ(net_switch::route(f, 64, mac, host), 0u);
    CHECK_EQ(net_switch::route(f, 10, mac, host), 0u);
}
