// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/pi_overclock.h — the "Pi CPU speed" setting
// (pi_overclock in g3macpi.ini) as a block of the Raspberry Pi firmware's
// config.txt, which sets the ARM clock at power-on (the emulator asks the
// firmware for its maximum: Circle's CCPUThrottle, CPUSpeedMaximum).
//
// The block is kept at the end of config.txt between two marker comments,
// in a [pi4] section (Pi 4, Pi 400, CM4), and nothing else in the file is
// touched:
//
//   # >>> G3MacPi CPU speed (the settings menu writes this; ...)
//   [pi4]
//   arm_freq=2000
//   over_voltage=6
//   [all]
//   # <<< G3MacPi CPU speed
//
// Limits (Raspberry Pi's config.txt documentation, overclocking): the Pi
// 400 runs its ARM cores at 1800 MHz by default; over_voltage above 6 needs
// force_turbo, which sets a permanent "warranty bit" in the SoC. This
// block uses neither force_turbo nor over_voltage above 6. 2000 MHz at
// over_voltage 6 is a common choice, not a figure from that documentation;
// the firmware slows the cores down again if the SoC gets too hot.
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_PI_OVERCLOCK_H
#define G3_PLATFORM_COMMON_PI_OVERCLOCK_H

#include "core/common/types.h"

namespace g3 {

namespace pi_overclock {

constexpr u32 kMhz = 2000;        // arm_freq with the overclock
constexpr u32 kOverVoltage = 6;   // over_voltage: the largest value without force_turbo

// Whether config.txt holds our block.
bool present(const char* text, u32 len);

// config.txt with our block removed and, if `on`, added at the end. Returns
// the new length, or 0 if it does not fit in `cap` (out is then unchanged
// in meaning). `changed`: the result differs from the input.
u32 apply(const char* text, u32 len, bool on, char* out, u32 cap, bool& changed);

} // namespace pi_overclock
} // namespace g3

#endif // G3_PLATFORM_COMMON_PI_OVERCLOCK_H
