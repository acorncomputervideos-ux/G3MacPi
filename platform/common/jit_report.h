// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/jit_report.h — what the JIT does with the Mac's code, in
// one line: the share of instructions run as generated code, handed to
// handlers from generated code, and run without generated code; and the
// kinds handed to handlers most (the candidates to compile next). From the
// CPU's JIT profile (Ppc750::jit_profile, docs/cpu/jit-a64.md).
//
// Example: "JIT: 71.2% native, 21.0% handlers, 7.8% interpreted; most
// handled: mtspr 6.1%, lwzx 4.0%, rlwimi 2.2%"
//
// Freestanding.

#ifndef G3_PLATFORM_COMMON_JIT_REPORT_H
#define G3_PLATFORM_COMMON_JIT_REPORT_H

#include "core/common/types.h"
#include "core/cpu/ppc750/ppc750.h"

namespace g3 {

struct JitReport {
    static constexpr u32 kTop = 5;
    u64 total = 0;                   // instructions executed
    u64 handlers = 0, interpreted = 0, native = 0;
    struct Kind {
        char name[12];               // mnemonic, e.g. "mtspr"
        u64 count;
    } top[kTop] = {};
    u32 ntop = 0;
};

void make_jit_report(const ppc::Ppc750& cpu, JitReport& out);

// The line above into out (zero-terminated); its length. "" if nothing
// has run yet.
u32 format_jit_report(const JitReport& r, char* out, u32 cap);

} // namespace g3

#endif // G3_PLATFORM_COMMON_JIT_REPORT_H
