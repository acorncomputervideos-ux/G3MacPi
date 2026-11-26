// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/wifi_scan.cpp — see wifi_scan.h.

#include "platform/common/wifi_scan.h"

namespace g3 {

namespace {

constexpr u32 kHeader = 12;              // buflen, version, sync_id, bss_count
constexpr u32 kBssVersion = 109;
constexpr u32 kOffCapability = 16, kOffSsidLen = 18, kOffSsid = 19, kOffChanspec = 72, kOffRssi = 78,
              kOffIeOffset = 116, kOffIeLength = 120, kBssMin = 124;
constexpr u16 kCapPrivacy = 0x0010;
constexpr u8 kIeRsn = 48, kIeVendor = 221;

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }

// An RSN element's body: version (2), group cipher (4), pairwise count (2)
// and suites (4 each), AKM count (2) and suites (4 each). True if every
// AKM suite is SAE (00-0F-AC:8): a WPA3-only network.
bool rsn_sae_only(const u8* b, u32 n)
{
    u32 p = 2 + 4;
    if (p + 2 > n)
        return false;
    p += 2 + 4u * le16(b + p);
    if (p + 2 > n)
        return false;
    const u32 akms = le16(b + p);
    p += 2;
    if (akms == 0 || p + 4 * akms > n)
        return false;
    for (u32 i = 0; i < akms; ++i, p += 4)
        if (!(b[p] == 0x00 && b[p + 1] == 0x0F && b[p + 2] == 0xAC && b[p + 3] == 8))
            return false;
    return true;
}

WifiNetwork::Security security_of(const u8* ie, u32 n, bool privacy)
{
    bool wpa = false;
    for (u32 p = 0; p + 2 <= n;) {
        const u8 id = ie[p], len = ie[p + 1];
        if (p + 2 + len > n)
            break;
        const u8* body = ie + p + 2;
        if (id == kIeRsn)
            return rsn_sae_only(body, len) ? WifiNetwork::Security::Wpa3 : WifiNetwork::Security::Wpa2;
        if (id == kIeVendor && len >= 4 && body[0] == 0x00 && body[1] == 0x50 && body[2] == 0xF2 && body[3] == 1)
            wpa = true;
        p += 2 + len;
    }
    if (wpa)
        return WifiNetwork::Security::Wpa;
    return privacy ? WifiNetwork::Security::Wep : WifiNetwork::Security::Open;
}

} // namespace

u32 WifiScanList::add_message(const u8* msg, u32 len)
{
    if (!msg || len < kHeader)
        return 0;
    const u32 count = le16(msg + 10);
    u32 found = 0;
    u32 off = kHeader;
    for (u32 i = 0; i < count; ++i) {
        if (off + kBssMin > len)
            break;
        const u8* b = msg + off;
        const u32 blen = le32(b + 4);
        if (le32(b) != kBssVersion || blen < kBssMin || off + blen > len)
            break;
        WifiNetwork n;
        u32 slen = b[kOffSsidLen];
        if (slen > 32)
            slen = 32;
        for (u32 k = 0; k < slen; ++k)
            n.ssid[k] = static_cast<char>(b[kOffSsid + k]);
        n.ssid[slen] = 0;
        n.rssi = static_cast<s16>(le16(b + kOffRssi));
        n.channel = static_cast<u8>(le16(b + kOffChanspec) & 0xFF);
        const u32 ie_off = le16(b + kOffIeOffset), ie_len = le32(b + kOffIeLength);
        const bool privacy = (le16(b + kOffCapability) & kCapPrivacy) != 0;
        if (ie_off <= blen && ie_len <= blen - ie_off)
            n.security = security_of(b + ie_off, ie_len, privacy);
        else
            n.security = privacy ? WifiNetwork::Security::Wep : WifiNetwork::Security::Open;
        add(n);
        ++found;
        off += blen;
    }
    return found;
}

void WifiScanList::add(const WifiNetwork& n)
{
    // Hidden networks send an empty name, or one of zero bytes.
    if (n.ssid[0] == 0)
        return;
    u32 i = 0;
    for (; i < count_; ++i) {
        const char* a = list_[i].ssid;
        const char* b = n.ssid;
        while (*a && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == *b)
            break;
    }
    if (i < count_) {
        if (n.rssi <= list_[i].rssi)
            return;  // a weaker access point of a network already listed
        // Stronger: take it out and insert it again below.
        for (u32 k = i; k + 1 < count_; ++k)
            list_[k] = list_[k + 1];
        --count_;
    }
    // Insert in order, strongest first; when full, the weakest goes.
    u32 pos = 0;
    while (pos < count_ && list_[pos].rssi >= n.rssi)
        ++pos;
    if (pos >= kMax)
        return;
    const u32 last = count_ < kMax ? count_ : kMax - 1;
    for (u32 k = last; k > pos; --k)
        list_[k] = list_[k - 1];
    list_[pos] = n;
    if (count_ < kMax)
        ++count_;
}

const char* WifiScanList::security_name(WifiNetwork::Security s)
{
    switch (s) {
    case WifiNetwork::Security::Open: return "Open";
    case WifiNetwork::Security::Wep: return "WEP";
    case WifiNetwork::Security::Wpa: return "WPA";
    case WifiNetwork::Security::Wpa2: return "WPA2";
    case WifiNetwork::Security::Wpa3: return "WPA3";
    }
    return "?";
}

u32 WifiScanList::bars(s16 rssi)
{
    return rssi >= -50 ? 4 : rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -85 ? 1 : 0;
}

void WifiScanList::printable(const char* ssid, char* out, u32 out_size)
{
    if (out_size == 0)
        return;
    u32 i = 0;
    for (; ssid[i] && i + 1 < out_size; ++i) {
        const u8 c = static_cast<u8>(ssid[i]);
        out[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?';
    }
    out[i] = 0;
}

} // namespace g3
