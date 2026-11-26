// SPDX-License-Identifier: GPL-3.0-or-later
// The modem's speaker (platform/common/modem_sound.h): the frequencies of
// each sound, measured with the Goertzel algorithm, and the speaker modes.

#include "platform/common/modem_sound.h"
#include "tests/framework/g3test.h"

#include <cmath>
#include <vector>

using namespace g3;
using Event = HayesModem::Event;

namespace {
constexpr u32 kRate = 48000;

std::vector<s16> render(ModemSound& s, u32 frames)
{
    std::vector<s16> out(2 * frames, 0);
    s.render_add(out.data(), frames, kRate);
    return out;
}
// Relative power at frequency f in the left channel (Goertzel).
double power(const std::vector<s16>& st, double f, size_t from = 0, size_t n = 0)
{
    if (!n)
        n = st.size() / 2 - from;
    const double w = 2 * M_PI * f / kRate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s = st[2 * (from + i)] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (static_cast<double>(n) * n);
}
double rms(const std::vector<s16>& st, size_t from = 0, size_t n = 0)
{
    if (!n)
        n = st.size() / 2 - from;
    double sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += static_cast<double>(st[2 * (from + i)]) * st[2 * (from + i)];
    return std::sqrt(sum / static_cast<double>(n));
}
} // namespace

TEST_CASE("platform/modem_sound/dtmf_digits")
{
    const char* keys = "123A456B789C*0#D";
    const double rows[4] = {697, 770, 852, 941}, cols[4] = {1209, 1336, 1477, 1633};
    for (u32 k = 0; k < 16; ++k) {
        ModemSound s;
        s.post(Event::Digit, keys[k], false, 1, 2);
        const std::vector<s16> out = render(s, kRate / 10);  // 100 ms
        const double lo = power(out, rows[k / 4]), hi = power(out, cols[k % 4]);
        CHECK(lo > 1e5);
        CHECK(hi > 1e5);
        for (u32 r = 0; r < 4; ++r)
            if (r != k / 4)
                CHECK(power(out, rows[r]) < lo / 50);
        for (u32 c = 0; c < 4; ++c)
            if (c != k % 4)
                CHECK(power(out, cols[c]) < hi / 50);
    }
}

TEST_CASE("platform/modem_sound/dial_tone_ringback_answer")
{
    ModemSound s;
    s.post(Event::OffHook, 0, false, 1, 2);
    std::vector<s16> out = render(s, kRate / 2);
    CHECK(power(out, 350) > 1e5);
    CHECK(power(out, 440) > 1e5);
    CHECK(power(out, 480) < power(out, 440) / 50);
    // Ringback: 440 + 480 Hz for 2 s, then 4 s of silence.
    s.post(Event::Ringback, 0, false, 1, 2);
    out = render(s, 6 * kRate);
    CHECK(power(out, 480, 0, kRate) > 1e5);
    CHECK(power(out, 440, 0, kRate) > 1e5);
    CHECK(rms(out, 2 * kRate + 100, 3 * kRate) < 1.0);
    // The handshake starts with the 2100 Hz answer tone.
    s.post(Event::Handshake, 0, false, 1, 2);
    out = render(s, kRate);
    CHECK(power(out, 2100, 0, kRate / 4) > 1e5);
    CHECK(power(out, 1500, 0, kRate / 4) < power(out, 2100, 0, kRate / 4) / 50);
    // ... and later is noise (training), loud and broad.
    out = render(s, 8 * kRate);
    CHECK(rms(out, 6 * kRate, kRate) > 500);
}

TEST_CASE("platform/modem_sound/speaker_modes_and_volume")
{
    // ATM0: silent.
    ModemSound off;
    off.post(Event::OffHook, 0, false, 0, 3);
    CHECK(rms(render(off, kRate / 10)) == 0);
    // ATM1: silent once connected; ATM2: a quiet hiss.
    ModemSound m1;
    m1.post(Event::Connected, 0, false, 1, 3);
    CHECK(rms(render(m1, kRate / 10)) == 0);
    ModemSound m2;
    m2.post(Event::Connected, 0, false, 2, 3);
    const double hiss = rms(render(m2, kRate / 10));
    CHECK(hiss > 10);
    // Louder at L3 than at L1.
    ModemSound quiet, loud;
    quiet.post(Event::OffHook, 0, false, 1, 1);
    loud.post(Event::OffHook, 0, false, 1, 3);
    CHECK(rms(render(loud, kRate / 10)) > 2 * rms(render(quiet, kRate / 10)));
    CHECK(hiss < rms(render(loud, kRate / 10)));
    // Pulse dialling: clicks, 10 a second, not DTMF.
    ModemSound p;
    p.post(Event::Digit, '5', true, 1, 2);
    const std::vector<s16> out = render(p, kRate / 2);
    CHECK(power(out, 770) < 1e4);
    CHECK(rms(out, 0, kRate / 200) > 100);                  // a click at the start
    CHECK(rms(out, kRate / 20, kRate / 40) == 0);           // quiet between clicks
}

TEST_CASE("platform/modem_sound/mixing_and_new_events")
{
    // Added to what is there, clipped (not wrapped).
    ModemSound s;
    s.post(Event::OffHook, 0, false, 1, 3);
    std::vector<s16> out(2 * 480, 32000);
    s.render_add(out.data(), 480, kRate);
    for (s16 v : out)
        CHECK(v >= 32000 - 9200);                           // never wrapped to negative
    bool clipped = false;
    for (s16 v : out)
        clipped |= v == 32767;
    CHECK(clipped);
    // A new event starts its sound from the beginning.
    s.post(Event::Digit, '1', false, 1, 2);
    std::vector<s16> d = render(s, kRate / 10);
    CHECK(power(d, 697) > 1e5);
    CHECK(power(d, 350) < power(d, 697) / 50);
    s.post(Event::OnHook, 0, false, 1, 2);
    CHECK(rms(render(s, kRate / 10)) == 0);
}
