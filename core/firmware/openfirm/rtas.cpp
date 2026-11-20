// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/rtas.cpp — see rtas.h.

#include "core/firmware/openfirm/rtas.h"

#include "core/common/cstring.h"

namespace g3::of {

namespace {

const char* const kNames[Rtas::kTokenCount] = {
    nullptr, "get-time-of-day", "set-time-of-day", "nvram-fetch", "nvram-store", "display-character",
    "power-off", "system-reboot", "read-pci-config", "write-pci-config",
};

// Days since 1970-01-01 for a civil date, and back (the standard
// era-based algorithm for the proleptic Gregorian calendar).
s64 days_from_civil(s64 y, u32 m, u32 d)
{
    y -= m <= 2;
    const s64 era = (y >= 0 ? y : y - 399) / 400;
    const u32 yoe = static_cast<u32>(y - era * 400);
    const u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<s64>(doe) - 719468;
}

void civil_from_days(s64 z, s64& y, u32& m, u32& d)
{
    z += 719468;
    const s64 era = (z >= 0 ? z : z - 146096) / 146097;
    const u32 doe = static_cast<u32>(z - era * 146097);
    const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<s64>(yoe) + era * 400;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u32 mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

bool leap(u32 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

} // namespace

void Rtas::to_calendar(u64 seconds, u32& year, u32& month, u32& day, u32& hour, u32& min, u32& sec)
{
    const s64 days = static_cast<s64>(seconds / 86400);
    const u32 rest = static_cast<u32>(seconds % 86400);
    s64 y;
    civil_from_days(days, y, month, day);
    year = static_cast<u32>(y);
    hour = rest / 3600;
    min = rest / 60 % 60;
    sec = rest % 60;
}

bool Rtas::from_calendar(u32 year, u32 month, u32 day, u32 hour, u32 min, u32 sec, u64& seconds)
{
    static const u8 kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || hour > 23 || min > 59 || sec > 59)
        return false;
    if (day > kDays[month - 1] + (month == 2 && leap(year) ? 1u : 0u))
        return false;
    seconds = static_cast<u64>(days_from_civil(year, month, day)) * 86400 + hour * 3600u + min * 60u + sec;
    return true;
}

Phandle Rtas::add_node(DeviceTree& t, Phandle root)
{
    const Phandle n = t.add_node(root, "rtas");
    if (n == kNone)
        return kNone;
    t.set_int(n, "rtas-size", kSize);
    t.set_int(n, "rtas-version", kVersion);
    for (u32 tok = 1; tok < kTokenCount; ++tok)
        t.set_int(n, kNames[tok], tok);
    return n;
}

bool Rtas::method(u32, const char* name, forth::Forth& f)
{
    if (!cstr::equal(name, "instantiate-rtas"))
        return false;
    const u32 at = f.pop();  // ( rtas-base-address -- rtas-call )
    if (f.failed())
        return true;
    if (at & 3 || !f.memory().write32(at, trap_word_)) {
        f.fail(-1, "instantiate-rtas: cannot write at that address");
        return true;
    }
    base_ = at;
    f.push(at);
    return true;
}

bool Rtas::call(ppc::MemoryInterface& mem, u32 args)
{
    ++calls_;
    u32 token = 0, nargs = 0, nret = 0;
    if (!mem.read32(args, token) || !mem.read32(args + 4, nargs) || !mem.read32(args + 8, nret) || nargs > 16 ||
        nret > 16)
        return false;
    u32 in[16] = {};
    for (u32 i = 0; i < nargs; ++i)
        if (!mem.read32(args + 12 + 4 * i, in[i]))
            return false;
    u32 out[16] = {};
    u32 nout = 1;  // the status, then results
    auto status = [&](s32 s) { out[0] = static_cast<u32>(s); };
    auto shape = [&](u32 a, u32 r) { return nargs == a && nret >= r; };
    status(kSuccess);

    switch (token) {
    case kGetTimeOfDay: {
        if (!shape(0, 8) || !h_.seconds) {
            status(kParameterError);
            break;
        }
        u32 y, mo, d, hh, mi, ss;
        to_calendar(h_.seconds(h_.ctx), y, mo, d, hh, mi, ss);
        const u32 v[7] = {y, mo, d, hh, mi, ss, 0};
        for (u32 i = 0; i < 7; ++i)
            out[1 + i] = v[i];
        nout = 8;
        break;
    }
    case kSetTimeOfDay: {
        u64 s = 0;
        if (!shape(7, 1) || !h_.set_seconds || !from_calendar(in[0], in[1], in[2], in[3], in[4], in[5], s))
            status(kParameterError);
        else
            h_.set_seconds(h_.ctx, s);
        break;
    }
    case kNvramFetch:
    case kNvramStore: {
        if (!shape(3, 2) || !h_.nvram_read || !h_.nvram_write || in[0] >= h_.nvram_size) {
            status(kParameterError);
            break;
        }
        const u32 index = in[0], buf = in[1];
        const u32 n = in[2] < h_.nvram_size - index ? in[2] : h_.nvram_size - index;
        u32 done = 0;
        for (; done < n; ++done) {
            if (token == kNvramFetch) {
                if (!mem.write8(buf + done, h_.nvram_read(h_.ctx, index + done)))
                    break;
            } else {
                u8 b = 0;
                if (!mem.read8(buf + done, b))
                    break;
                h_.nvram_write(h_.ctx, index + done, b);
            }
        }
        if (done < n)
            status(kHardwareError);
        out[1] = done;
        nout = 2;
        break;
    }
    case kDisplayCharacter:
        if (!shape(1, 1))
            status(kParameterError);
        else if (h_.put_char)
            h_.put_char(h_.ctx, static_cast<char>(in[0]));
        break;
    case kPowerOff:
    case kSystemReboot:
        if (token == kPowerOff ? !shape(2, 1) : !shape(0, 1))
            status(kParameterError);
        else if (h_.power)
            h_.power(h_.ctx, token == kSystemReboot);
        break;
    case kReadPciConfig:
    case kWritePciConfig: {
        const bool rd = token == kReadPciConfig;
        const u32 addr = in[0], size = in[1];
        const u32 reg = ((addr >> 20) & 0xF00) | (addr & 0xFF), bus = (addr >> 16) & 0xFF, devfn = (addr >> 8) & 0xFF;
        if (!(rd ? shape(2, 2) : shape(3, 1)) || !h_.pci_read || !h_.pci_write ||
            (size != 1 && size != 2 && size != 4) || reg % size || reg >= 0x100) {
            status(kParameterError);
            if (rd)
                out[1] = 0xFFFFFFFFu, nout = 2;
            break;
        }
        const u32 shift = 8 * (reg & 3);
        const u32 mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1);
        if (rd) {
            out[1] = (h_.pci_read(h_.ctx, bus, devfn, reg & 0xFC) >> shift) & mask;
            nout = 2;
        } else {
            const u32 enables = ((1u << size) - 1) << (reg & 3);
            h_.pci_write(h_.ctx, bus, devfn, reg & 0xFC, (in[2] & mask) << shift, enables);
        }
        break;
    }
    default:
        status(kHardwareError);  // not a token of ours
        break;
    }
    for (u32 i = 0; i < nret && i < nout; ++i)
        if (!mem.write32(args + 12 + 4 * (nargs + i), out[i]))
            return false;
    return true;
}

} // namespace g3::of
