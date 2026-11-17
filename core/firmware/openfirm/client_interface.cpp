// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/client_interface.cpp — see client_interface.h.

#include "core/firmware/openfirm/client_interface.h"

#include "core/common/cstring.h"

namespace g3::of {

bool ClientInterface::read_string(u32 addr, char* out, u32 max)
{
    for (u32 i = 0; i < max; ++i) {
        u8 c = 0;
        if (!mem_.read8(addr + i, c))
            return false;
        out[i] = static_cast<char>(c);
        if (c == 0)
            return true;
    }
    return false;
}

bool ClientInterface::write_bytes(u32 addr, const u8* data, u32 len)
{
    for (u32 i = 0; i < len; ++i)
        if (!mem_.write8(addr + i, data[i]))
            return false;
    return true;
}

void ClientInterface::ret(u32 i, u32 v)
{
    if (i < nrets_)
        mem_.write32(args_addr_ + 12 + 4 * (nargs_ + i), v);
}

u32 ClientInterface::call(u32 args_addr)
{
    args_addr_ = args_addr;
    u32 name_addr = 0;
    service_[0] = '\0';
    if (!read_cell(args_addr, name_addr) || !read_cell(args_addr + 4, nargs_) ||
        !read_cell(args_addr + 8, nrets_) || nargs_ > kMaxArgs ||
        !read_string(name_addr, service_, sizeof service_)) {
        ++unknown_calls_;
        return kFail;
    }
    for (u32 i = 0; i < nargs_; ++i)
        if (!read_cell(args_addr + 12 + 4 * i, args_[i]))
            return kFail;
    if (!dispatch()) {
        ++unknown_calls_;
        return kFail;
    }
    return 0;
}

bool ClientInterface::dispatch()
{
    const char* s = service_;
    auto is = [s](const char* name) { return cstr::equal(s, name); };
    auto need = [this](u32 n, u32 r) { return nargs_ >= n && nrets_ >= r; };
    char name[kMaxString];

    if (is("test")) {  // ( name -- missing? )
        if (!need(1, 1))
            return false;
        static const char* const known[] = {
            "test", "peer", "child", "parent", "getproplen", "getprop", "nextprop",
            "setprop", "finddevice", "canon", "package-to-path", "milliseconds", "exit",
            "enter",
        };
        u32 missing = kFail;
        if (read_string(arg(0), name, sizeof name))
            for (const char* k : known)
                if (cstr::equal(name, k))
                    missing = 0;
        ret(0, missing);
        return true;
    }
    if (is("peer") || is("child") || is("parent")) {  // ( phandle -- phandle' )
        if (!need(1, 1))
            return false;
        const Phandle h = arg(0);
        Phandle r;
        if (h != kNone && !tree_.valid(h))
            r = kFail;
        else if (s[0] == 'p' && s[1] == 'e')
            r = tree_.peer(h);  // peer(0) = root
        else if (h == kNone)
            r = kFail;
        else if (s[0] == 'c')
            r = tree_.child(h);
        else
            r = tree_.parent(h);
        ret(0, r);
        return true;
    }
    if (is("getproplen")) {  // ( phandle name -- len )
        if (!need(2, 1))
            return false;
        const u8* data = nullptr;
        u32 len = 0;
        const bool found = read_string(arg(1), name, sizeof name) &&
                           tree_.get_property(arg(0), name, data, len);
        ret(0, found ? len : kFail);
        return true;
    }
    if (is("getprop")) {  // ( phandle name buf buflen -- size )
        if (!need(4, 1))
            return false;
        const u8* data = nullptr;
        u32 len = 0;
        if (!read_string(arg(1), name, sizeof name) || !tree_.get_property(arg(0), name, data, len)) {
            ret(0, kFail);
            return true;
        }
        const u32 n = len < arg(3) ? len : arg(3);
        ret(0, write_bytes(arg(2), data, n) ? len : kFail);
        return true;
    }
    if (is("nextprop")) {  // ( phandle previous buf -- flag )
        if (!need(3, 1))
            return false;
        if (!tree_.valid(arg(0))) {
            ret(0, kFail);
            return true;
        }
        const char* prev = nullptr;
        if (arg(1) != 0) {
            if (!read_string(arg(1), name, sizeof name)) {
                ret(0, kFail);
                return true;
            }
            prev = name;
        }
        bool missing = false;
        const char* next = tree_.next_property(arg(0), prev, &missing);
        if (missing) {
            ret(0, kFail);
        } else if (next == nullptr) {
            const u8 empty = 0;
            write_bytes(arg(2), &empty, 1);
            ret(0, 0);
        } else {
            ret(0, write_bytes(arg(2), reinterpret_cast<const u8*>(next), cstr::length(next) + 1)
                       ? 1u : kFail);
        }
        return true;
    }
    if (is("setprop")) {  // ( phandle name buf len -- size )
        if (!need(4, 1))
            return false;
        u8* value = setprop_buf_;
        const u32 len = arg(3);
        bool ok = len <= sizeof setprop_buf_ && read_string(arg(1), name, sizeof name);
        for (u32 i = 0; ok && i < len; ++i)
            ok = mem_.read8(arg(2) + i, value[i]);
        ok = ok && tree_.set_property(arg(0), name, value, len);
        ret(0, ok ? len : kFail);
        return true;
    }
    if (is("finddevice")) {  // ( device-specifier -- phandle )
        if (!need(1, 1))
            return false;
        Phandle h = kNone;
        if (read_string(arg(0), name, sizeof name))
            h = tree_.find(name);
        ret(0, h == kNone ? kFail : h);
        return true;
    }
    if (is("canon")) {  // ( device-specifier buf buflen -- length )
        if (!need(3, 1))
            return false;
        Phandle h = kNone;
        if (read_string(arg(0), name, sizeof name))
            h = tree_.find(name);
        if (h == kNone) {
            ret(0, kFail);
            return true;
        }
        char path[kMaxString];
        const u32 len = tree_.path_of(h, path, sizeof path);
        const u32 n = len + 1 < arg(2) ? len + 1 : arg(2);  // with the NUL if it fits
        ret(0, len < sizeof path && write_bytes(arg(1), reinterpret_cast<const u8*>(path), n)
                   ? len : kFail);
        return true;
    }
    if (is("package-to-path")) {  // ( phandle buf buflen -- length )
        if (!need(3, 1))
            return false;
        if (!tree_.valid(arg(0))) {
            ret(0, kFail);
            return true;
        }
        char path[kMaxString];
        const u32 len = tree_.path_of(arg(0), path, sizeof path);
        const u32 n = len + 1 < arg(2) ? len + 1 : arg(2);
        ret(0, len < sizeof path && write_bytes(arg(1), reinterpret_cast<const u8*>(path), n)
                   ? len : kFail);
        return true;
    }
    if (is("milliseconds")) {  // ( -- ms )
        if (!need(0, 1))
            return false;
        ret(0, clock_cb_ ? clock_cb_(clock_ctx_) : 0);
        return true;
    }
    if (is("exit")) {
        if (exit_cb_)
            exit_cb_(exit_ctx_);
        return true;
    }
    if (is("enter")) {
        if (enter_cb_)
            enter_cb_(enter_ctx_);
        return true;
    }
    return false;
}

} // namespace g3::of
