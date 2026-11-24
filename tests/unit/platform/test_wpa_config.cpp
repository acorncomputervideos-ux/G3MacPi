// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_wpa_config.cpp — the WiFi settings as a
// wpa_supplicant.conf (platform/common/wpa_config).

#include "platform/common/wpa_config.h"
#include "tests/framework/g3test.h"

#include <cstring>

using namespace g3;

TEST_CASE("platform/wpaconfig/text")
{
    MachineConfig c;
    char out[512];
    CHECK_EQ(write_wpa_config(c, out, sizeof out), 0u);  // no network name
    CHECK_EQ(out[0], 0);
    std::strcpy(c.wifi_name, "Me \"#1\"");
    std::strcpy(c.wifi_password, "pa ss#word");
    std::strcpy(c.wifi_country, "GB");
    const u32 n = write_wpa_config(c, out, sizeof out);
    CHECK(n > 0);
    CHECK_EQ(std::strlen(out), n);
    CHECK(std::strstr(out, "country=GB\n") != nullptr);
    CHECK(std::strstr(out, "\tssid=4d652022233122\n") != nullptr);  // hex of: Me "#1"
    CHECK(std::strstr(out, "\tpsk=\"pa ss#word\"\n") != nullptr);
    CHECK(std::strstr(out, "key_mgmt=WPA-PSK") != nullptr);
    CHECK(std::strstr(out, "network={\n") != nullptr);
    CHECK(out[n - 2] == '}');
    // Open network, no country.
    c.wifi_password[0] = '\0';
    c.wifi_country[0] = '\0';
    write_wpa_config(c, out, sizeof out);
    CHECK(std::strstr(out, "key_mgmt=NONE") != nullptr);
    CHECK(std::strstr(out, "psk=") == nullptr);
    CHECK(std::strstr(out, "country=") == nullptr);
    // Too small a buffer: nothing.
    char small[40];
    CHECK_EQ(write_wpa_config(c, small, sizeof small), 0u);
    CHECK_EQ(small[0], 0);
}
