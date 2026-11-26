// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/wifi_scan.h — the WiFi networks around the Pi, from the
// Pi's radio's scan results, for the settings menu's "Search for networks".
//
// Input: the radio firmware's "escan result" messages as Circle's WLAN
// driver queues them (CBcm4343Device::ReceiveScanResult): a 12-byte header
// (buflen u32, version u32, sync_id u16, bss_count u16), then bss_count
// BSS records, each starting with version u32 and its own length u32. All
// little-endian. In a record (Broadcom's bss_info, version 109): BSSID at 8,
// capability u16 at 16 (bit 4: privacy), SSID length at 18 and the SSID at
// 19 (up to 32 bytes), chanspec u16 at 72 (low byte: the channel), RSSI
// s16 at 78 (dBm), the information elements' offset u16 at 116 and length
// u32 at 120. Sources (facts only): Circle's addon/wlan, the record layout
// in hostap/src/drivers/driver_circle.cpp and its parser in ether4330.c.
//
// Security, from the information elements (IEEE 802.11): an RSN element
// (ID 48) means WPA2, or WPA3 when its only key management is SAE (AKM
// suite 00-0F-AC:8); else a vendor element (ID 221) 00-50-F2 type 1 means
// WPA; else the privacy bit means WEP; else the network is open.
//
// The list keeps one entry per network name (the strongest access point),
// strongest first, without hidden networks (no name).

#ifndef G3_PLATFORM_COMMON_WIFI_SCAN_H
#define G3_PLATFORM_COMMON_WIFI_SCAN_H

#include "core/common/types.h"

namespace g3 {

struct WifiNetwork {
    enum class Security : u8 { Open, Wep, Wpa, Wpa2, Wpa3 };
    char ssid[33] = {};  // the name's bytes, 0-terminated
    s16 rssi = 0;        // dBm (e.g. -45 strong, -85 weak)
    u8 channel = 0;
    Security security = Security::Open;
};

class WifiScanList {
public:
    static constexpr u32 kMax = 32;

    void clear() { count_ = 0; }
    // One message from the radio. Returns the number of networks it
    // described (0 for the last, empty message or a malformed one).
    u32 add_message(const u8* msg, u32 len);
    // Adds or updates one network (ignored without a name).
    void add(const WifiNetwork& n);

    u32 count() const { return count_; }
    const WifiNetwork& at(u32 i) const { return list_[i]; }

    // "Open", "WEP", "WPA", "WPA2", "WPA3".
    static const char* security_name(WifiNetwork::Security s);
    // 0..4 bars from the signal (-50 dBm and above: 4; below -85: 0).
    static u32 bars(s16 rssi);
    // The name as text for the menu: bytes outside printable ASCII as '?'.
    static void printable(const char* ssid, char* out, u32 out_size);

private:
    WifiNetwork list_[kMax];
    u32 count_ = 0;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_WIFI_SCAN_H
