// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/modem_sound.cpp — see modem_sound.h.

#include "platform/common/modem_sound.h"

namespace g3 {

namespace {
using Event = HayesModem::Event;

// sin(2*pi*x) for x in [0, 1), by its Taylor series around the nearest
// quarter (enough terms for 16-bit audio).
float sin_turns(double x)
{
    const double pi2 = 6.283185307179586;
    double a = (x - static_cast<double>(static_cast<long long>(x))) * pi2;  // [0, 2pi)
    if (a > 3.141592653589793)
        a -= pi2;  // [-pi, pi]
    const double a2 = a * a;
    double term = a, sum = a;
    for (int k = 1; k < 12; ++k) {
        term *= -a2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return static_cast<float>(sum);
}

bool dtmf(char d, float& low, float& high)
{
    static const char kKeys[] = "123A456B789C*0#D";
    static const float kRow[4] = {697, 770, 852, 941}, kCol[4] = {1209, 1336, 1477, 1633};
    for (u32 i = 0; i < 16; ++i)
        if (kKeys[i] == d) {
            low = kRow[i / 4];
            high = kCol[i % 4];
            return true;
        }
    return false;
}
} // namespace

ModemSound::ModemSound()
{
    for (u32 i = 0; i < 1024; ++i)
        table_[i] = sin_turns(static_cast<double>(i) / 1024.0);
}

float ModemSound::sine(float phase) const
{
    phase -= static_cast<float>(static_cast<long long>(phase));
    if (phase < 0)
        phase += 1;
    const float x = phase * 1024.0f;  // linear interpolation between table entries
    const u32 i = static_cast<u32>(x) & 1023;
    const float f = x - static_cast<float>(static_cast<u32>(x));
    return table_[i] + f * (table_[(i + 1) & 1023] - table_[i]);
}

void ModemSound::post(Event e, char digit, bool pulse, u8 speaker_mode, u8 volume)
{
    const u32 gen = (__atomic_load_n(&posted_, __ATOMIC_RELAXED) >> 16) + 1;
    __atomic_store_n(&settings_, static_cast<u32>(speaker_mode) | static_cast<u32>(volume) << 8 | (pulse ? 1u << 16 : 0),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&posted_, (gen & 0xFFFF) << 16 | static_cast<u32>(e) << 8 | static_cast<u8>(digit),
                     __ATOMIC_RELEASE);
}

float ModemSound::sample(float t)
{
    auto tone = [&](float f) { return sine(f * t); };
    auto noise = [&]() {
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        const float x = static_cast<float>(static_cast<s32>(noise_)) / 2147483648.0f;
        lp_ += 0.35f * (x - lp_);  // about 3.4 kHz at 48 kHz: the telephone band
        return lp_;
    };
    switch (event_) {
    case Event::OffHook:
        return 0.5f * (tone(350) + tone(440));
    case Event::Digit: {
        if (pulse_) {  // a short click at each pulse
            const float in = t * 10.0f - static_cast<float>(static_cast<int>(t * 10.0f));
            return in < 0.05f ? 0.8f * (1.0f - in / 0.05f) * (noise() > 0 ? 1.0f : -1.0f) : 0.0f;
        }
        float lo, hi;
        return dtmf(digit_, lo, hi) ? 0.5f * (tone(lo) + tone(hi)) : 0.0f;
    }
    case Event::Ringback: {
        const float in = t - 6.0f * static_cast<float>(static_cast<int>(t / 6.0f));
        return in < 2.0f ? 0.5f * (tone(440) + tone(480)) : 0.0f;
    }
    case Event::Handshake: {
        const float ms = t * 1000.0f;
        if (ms < kAnsamMs) {  // ANSam: 2100 Hz, 15 Hz AM, phase reversals every 450 ms
            const int reversals = static_cast<int>(t / 0.45f);
            const float s = tone(2100) * (1.0f + 0.2f * tone(15));
            return (reversals & 1) ? -0.8f * s : 0.8f * s;
        }
        if (ms < kAnsamMs + kV21Ms) {  // V.21: both channels, 300 bit/s, random bits
            const u32 bit = static_cast<u32>(t * 300.0f);
            const u32 r = bit * 2654435761u;
            const float f1 = (r & 0x10000) ? 980.0f : 1180.0f, f2 = (r & 0x20000) ? 1650.0f : 1850.0f;
            fsk_phase_[0] += f1 / static_cast<float>(rate_);
            fsk_phase_[1] += f2 / static_cast<float>(rate_);
            return 0.4f * (sine(fsk_phase_[0]) + sine(fsk_phase_[1]));
        }
        if (ms < kAnsamMs + kV21Ms + kProbeMs) {  // line probing: 150 Hz to 3750 Hz, every 150 Hz
            float s = 0;
            for (u32 k = 1; k <= 25; ++k)
                s += sine(150.0f * static_cast<float>(k) * t + static_cast<float>(k * k) * 0.137f);
            return 0.12f * s;
        }
        return 0.9f * noise();  // training
    }
    case Event::Connected:
        return speaker_ == 2 ? 0.15f * noise() : 0.0f;
    case Event::Pause:
    case Event::OnHook:
        return 0.0f;
    }
    return 0.0f;
}

void ModemSound::render_add(s16* out, u32 count, u32 rate_hz)
{
    const u32 posted = __atomic_load_n(&posted_, __ATOMIC_ACQUIRE);
    const u32 settings = __atomic_load_n(&settings_, __ATOMIC_RELAXED);
    if (posted != seen_) {
        seen_ = posted;
        event_ = static_cast<Event>((posted >> 8) & 0xFF);
        digit_ = static_cast<char>(posted & 0xFF);
        n_ = 0;
        fsk_phase_[0] = fsk_phase_[1] = 0;
    }
    speaker_ = static_cast<u8>(settings & 0xFF);
    volume_ = static_cast<u8>((settings >> 8) & 0xFF);
    pulse_ = (settings >> 16) & 1;
    rate_ = rate_hz ? rate_hz : 48000;
    if (speaker_ == 0 || (event_ == Event::Connected && speaker_ != 2) || event_ == Event::OnHook ||
        event_ == Event::Pause) {
        n_ += count;
        return;
    }
    static const float kGain[4] = {0.05f, 0.10f, 0.18f, 0.28f};
    const float gain = kGain[volume_ & 3] * 32767.0f;
    for (u32 i = 0; i < count; ++i) {
        const float t = static_cast<float>(n_++) / static_cast<float>(rate_);
        const s32 v = static_cast<s32>(sample(t) * gain);
        for (u32 c = 0; c < 2; ++c) {
            s32 m = out[2 * i + c] + v;
            m = m > 32767 ? 32767 : m < -32768 ? -32768 : m;
            out[2 * i + c] = static_cast<s16>(m);
        }
    }
}

} // namespace g3
