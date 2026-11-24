// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/unit/platform/test_pi_overclock.cpp — the "Pi CPU speed" block in
// config.txt (platform/common/pi_overclock): added, removed, idempotent,
// nothing else touched.

#include "platform/common/pi_overclock.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <string>

using namespace g3;

namespace {
std::string run(const std::string& in, bool on, bool* changed_out = nullptr)
{
    char out[4096];
    bool changed = false;
    const u32 n = pi_overclock::apply(in.data(), static_cast<u32>(in.size()), on, out, sizeof out, changed);
    if (changed_out)
        *changed_out = changed;
    return std::string(out, n);
}
} // namespace

TEST_CASE("platform/overclock/config_txt_block")
{
    const std::string base = "arm_64bit=1\n\n[pi4]\nkernel=kernel8-rpi4.img\n\n[all]\nenable_uart=1\n#arm_freq=2000\n";
    CHECK(!pi_overclock::present(base.data(), static_cast<u32>(base.size())));
    bool changed = false;
    const std::string on = run(base, true, &changed);
    CHECK(changed);
    CHECK(on.compare(0, base.size(), base) == 0);           // the original first, untouched
    CHECK(on.find("[pi4]\narm_freq=2000\nover_voltage=6\n[all]\n") != std::string::npos);
    CHECK(on.find("force_turbo") == std::string::npos);     // never: it sets the warranty bit
    CHECK(pi_overclock::present(on.data(), static_cast<u32>(on.size())));
    // Again: nothing changes (one block only).
    CHECK(run(on, true, &changed) == on);
    CHECK(!changed);
    // Off: exactly the original back.
    CHECK(run(on, false, &changed) == base);
    CHECK(changed);
    CHECK(run(base, false, &changed) == base);
    CHECK(!changed);
    // A file without a final newline: the block starts on its own line.
    const std::string bare = "arm_64bit=1";
    const std::string b2 = run(bare, true);
    CHECK(b2.compare(0, 12, "arm_64bit=1\n") == 0);
    CHECK(pi_overclock::present(b2.data(), static_cast<u32>(b2.size())));
    // Lines after our block (the user added some) are kept when it goes.
    const std::string later = on + "dtoverlay=something\n";
    const std::string off = run(later, false);
    CHECK(off == base + "dtoverlay=something\n");
    // Too small an output buffer: 0.
    char tiny[16];
    CHECK_EQ(pi_overclock::apply(base.data(), static_cast<u32>(base.size()), true, tiny, sizeof tiny, changed), 0u);
}
