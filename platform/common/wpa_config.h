// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/wpa_config.h — the WiFi settings (wifi_name,
// wifi_password, wifi_country in g3macpi.ini) as a wpa_supplicant
// configuration file, which Circle's WPA supplicant reads
// (third_party/circle/addon/wlan, a port of hostap's wpa_supplicant).
//
// The network name is written in hex (wpa_supplicant.conf: "ssid" is a
// quoted string or a hex string), so any name works, quotes and '#'
// included. The password is a quoted passphrase (8-63 characters; the
// configuration parser refuses '"' in it); none means an open network.
// Circle's port supports WPA/WPA2 personal (PSK), not WPA3 (SAE).
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_WPA_CONFIG_H
#define G3_PLATFORM_COMMON_WPA_CONFIG_H

#include "core/common/types.h"
#include "core/machine/machine_config.h"

namespace g3 {

// The file's text into out (zero-terminated); its length, or 0 if there is
// no network name or it does not fit.
u32 write_wpa_config(const MachineConfig& config, char* out, u32 cap);

} // namespace g3

#endif // G3_PLATFORM_COMMON_WPA_CONFIG_H
