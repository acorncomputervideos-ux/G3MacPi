// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/jit_report.cpp — see jit_report.h.

#include "platform/common/jit_report.h"
#include "core/cpu/ppc750/disasm.h"

namespace g3 {

namespace {

// The first word of `src` (up to a space) into out. Not inlined: GCC's
// array-bounds check misreads the unrolled loop for constant strings.
__attribute__((noinline)) void first_word(char* out, u32 cap, const char* src)
{
    u32 i = 0;
    for (; src[i] && src[i] != ' ' && i + 1 < cap; ++i)
        out[i] = src[i];
    out[i] = 0;
}

// The mnemonic of a kind, from the instruction word the profile kept.
void kind_name(u32 id, u32 sample, char* out, u32 cap)
{
    if (id == 64 + 1024 + 467) {          // disassembled as mtlr, mtctr ...: one kind here
        first_word(out, cap, "mtspr");
        return;
    }
    if (id == 64 + 1024 + 339) {
        first_word(out, cap, "mfspr");
        return;
    }
    char text[64];
    ppc::disassemble(0, sample, text, sizeof text);
    first_word(out, cap, text);
}

struct Out {
    char* p;
    u32 cap, n = 0;
    void c(char ch)
    {
        if (n + 1 < cap)
            p[n++] = ch;
    }
    void s(const char* t)
    {
        while (*t)
            c(*t++);
    }
    void num(u64 v)
    {
        char d[24];
        u32 k = 0;
        do {
            d[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k)
            c(d[--k]);
    }
    // part / whole as a percentage with one decimal.
    void pct(u64 part, u64 whole)
    {
        const u64 tenths = whole ? (part * 1000 + whole / 2) / whole : 0;
        num(tenths / 10);
        c('.');
        num(tenths % 10);
        c('%');
    }
};

} // namespace

void make_jit_report(const ppc::Ppc750& cpu, JitReport& r)
{
    r = JitReport{};
    r.total = cpu.instructions_executed();
    r.interpreted = cpu.jit_stats().interpreted;
    const u64* prof = cpu.jit_profile();
    for (u32 id = 0; id < ppc::Ppc750::kJitOpIds; ++id) {
        const u64 n = prof[id];
        if (n == 0)
            continue;
        r.handlers += n;
        // Insert into the top list (largest first).
        u32 at = r.ntop;
        while (at > 0 && r.top[at - 1].count < n)
            --at;
        if (at >= JitReport::kTop)
            continue;
        const u32 last = r.ntop < JitReport::kTop ? r.ntop : JitReport::kTop - 1;
        for (u32 k = last; k > at; --k)
            r.top[k] = r.top[k - 1];
        r.top[at].count = n;
        kind_name(id, cpu.jit_profile_sample(id), r.top[at].name, sizeof r.top[at].name);
        if (r.ntop < JitReport::kTop)
            ++r.ntop;
    }
    const u64 other = r.handlers + r.interpreted;
    r.native = r.total > other ? r.total - other : 0;
}

u32 format_jit_report(const JitReport& r, char* out, u32 cap)
{
    if (cap == 0)
        return 0;
    Out o{out, cap};
    if (r.total) {
        o.s("JIT: ");
        o.pct(r.native, r.total);
        o.s(" native, ");
        o.pct(r.handlers, r.total);
        o.s(" handlers, ");
        o.pct(r.interpreted, r.total);
        o.s(" interpreted");
        for (u32 k = 0; k < r.ntop; ++k) {
            o.s(k == 0 ? "; most handled: " : ", ");
            o.s(r.top[k].name);
            o.c(' ');
            o.pct(r.top[k].count, r.total);
        }
    }
    out[o.n] = 0;
    return o.n;
}

} // namespace g3
