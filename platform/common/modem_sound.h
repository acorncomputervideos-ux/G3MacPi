// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/modem_sound.h — the dial-up modem's speaker, generated
// (no recordings; docs/devices/modem.md). The modem (on the emulation
// core) posts its events; the core that plays audio mixes the sound of the
// current event into what it sends to the output.
//
// Sounds:
//  - Dial tone 350 + 440 Hz; ringback 440 + 480 Hz, 2 s on, 4 s off (the
//    North American Precise Tone Plan, ITU-T E.180 Supplement 2).
//  - DTMF digits (ITU-T Q.23): rows 697, 770, 852, 941 Hz; columns 1209,
//    1336, 1477, 1633 Hz. Pulse digits: a click per pulse, 10 a second.
//  - The handshake, an approximation of a V.90 call's sounds, in order:
//    the answer tone ANSam (2100 Hz, amplitude-modulated at 15 Hz, phase
//    reversed every 450 ms: V.25 / V.8), V.21 FSK signalling (980/1180 Hz
//    and 1650/1850 Hz at 300 bit/s: V.8's CM and JM), line probing (tones
//    every 150 Hz from 150 to 3750 Hz), then training (band-limited noise).
//  - Speaker modes (ATM): 0 silent, 1 until connected, 2 always (a quiet
//    carrier hiss while connected); volume (ATL) 0-3.
//
// Producer: post() (one core). Consumer: render_add() (another core).
// Freestanding.

#ifndef G3_PLATFORM_COMMON_MODEM_SOUND_H
#define G3_PLATFORM_COMMON_MODEM_SOUND_H

#include "core/common/types.h"
#include "core/devices/modem/hayes_modem.h"

namespace g3 {

class ModemSound {
public:
    ModemSound();
    // Producer: the modem's event, its digit, how it dials, its speaker.
    void post(HayesModem::Event e, char digit, bool pulse, u8 speaker_mode, u8 volume);

    // Consumer: adds the sound to count interleaved stereo frames at rate.
    void render_add(s16* out, u32 count, u32 rate_hz);

    // Handshake schedule (ms from its start), for tests and the docs.
    static constexpr u32 kAnsamMs = 2500, kV21Ms = 1000, kProbeMs = 1500;

private:
    float sample(float t);  // the current event's sound at t seconds into it
    float sine(float phase) const;  // phase in turns

    // Posted: generation (16 bits) | event (8) | digit (8); settings.
    u32 posted_ = 0, settings_ = 0;
    // Consumer state.
    u32 seen_ = 0xFFFFFFFFu;
    HayesModem::Event event_ = HayesModem::Event::OnHook;
    char digit_ = 0;
    bool pulse_ = false;
    u8 speaker_ = 1, volume_ = 1;
    u64 n_ = 0;            // samples since the event started
    u32 rate_ = 48000;
    u32 noise_ = 0x12345678u;
    float lp_ = 0;         // noise low-pass state
    float fsk_phase_[2] = {0, 0};
    float table_[1024];
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_MODEM_SOUND_H
