// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/wpa_config.cpp — see wpa_config.h.

#include "platform/common/wpa_config.h"

namespace g3 {

namespace {
struct Out {
    char* p;
    u32 cap, n = 0;
    bool ok = true;
    void c(char ch)
    {
        if (n + 1 < cap)
            p[n++] = ch;
        else
            ok = false;
    }
    void s(const char* t)
    {
        while (*t)
            c(*t++);
    }
};
} // namespace

u32 write_wpa_config(const MachineConfig& cfg, char* out, u32 cap)
{
    if (cap == 0)
        return 0;
    out[0] = '\0';
    if (cfg.wifi_name[0] == '\0')
        return 0;
    Out o{out, cap};
    o.s("# Written by G3MacPi from g3macpi.ini (wifi_name, wifi_password, wifi_country).\n");
    if (cfg.wifi_country[0]) {
        o.s("country=");
        o.s(cfg.wifi_country);
        o.s("\n");
    }
    o.s("\nnetwork={\n\tssid=");
    static const char kHex[] = "0123456789abcdef";
    for (const char* q = cfg.wifi_name; *q; ++q) {
        const u8 b = static_cast<u8>(*q);
        o.c(kHex[b >> 4]);
        o.c(kHex[b & 15]);
    }
    o.s("\n\tscan_ssid=1\n");  // also finds a hidden network
    // 2.4 GHz only (channels 1-13): on a Pi 400 the join of a dual-band
    // router's 5 GHz access point (a DFS channel, 5290 MHz) failed every
    // time, and the retries kept the Mac's core busy (docs/wifi.md).
    o.s("\tfreq_list=2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462 2467 2472\n");
    if (cfg.wifi_password[0]) {
        o.s("\tpsk=\"");
        o.s(cfg.wifi_password);
        o.s("\"\n\tkey_mgmt=WPA-PSK\n");
    } else {
        o.s("\tkey_mgmt=NONE\n");
    }
    o.s("}\n");
    if (!o.ok) {
        out[0] = '\0';
        return 0;
    }
    out[o.n] = '\0';
    return o.n;
}

} // namespace g3
