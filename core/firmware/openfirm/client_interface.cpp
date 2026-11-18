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
    const bool handled = dispatch();
    if (trace_)
        trace_(trace_ctx_, service_, args_addr, handled);
    if (!handled) {
        ++unknown_calls_;
        return kFail;
    }
    return 0;
}

bool ClientInterface::provides(const char* n) const
{
    static const char* const core[] = {
        "test", "peer", "child", "parent", "getproplen", "getprop", "nextprop",
        "setprop", "finddevice", "canon", "package-to-path", "milliseconds", "exit",
        "enter", "quiesce",
    };
    static const char* const memory[] = {"claim", "release"};
    static const char* const inst[] = {
        "open", "close", "read", "write", "seek", "instance-to-package", "instance-to-path",
    };
    for (const char* k : core)
        if (cstr::equal(n, k))
            return true;
    if (memory_)
        for (const char* k : memory)
            if (cstr::equal(n, k))
                return true;
    if (instances_)
        for (const char* k : inst)
            if (cstr::equal(n, k))
                return true;
    if (forth_ && (cstr::equal(n, "interpret") || cstr::equal(n, "call-method")))
        return true;
    return false;
}

u32 ClientInterface::copy_out(u32 addr, const char* text, u32 len, u32 buflen)
{
    const u32 n = len + 1 < buflen ? len + 1 : buflen;  // with the NUL if it fits
    return write_bytes(addr, reinterpret_cast<const u8*>(text), n) ? len : kFail;
}

bool ClientInterface::dispatch_memory()
{
    const char* s = service_;
    if (cstr::equal(s, "claim")) {  // ( virt size align -- base )
        if (nargs_ < 3 || nrets_ < 1)
            return false;
        ret(0, memory_->claim(arg(0), arg(1), arg(2)));
        return true;
    }
    if (cstr::equal(s, "release")) {  // ( virt size -- )
        if (nargs_ < 2)
            return false;
        memory_->release(arg(0), arg(1));
        return true;
    }
    return false;
}

bool ClientInterface::dispatch_instances()
{
    const char* s = service_;
    InstanceTable& t = *instances_;
    if (cstr::equal(s, "open")) {  // ( device-specifier -- ihandle )
        if (nargs_ < 1 || nrets_ < 1)
            return false;
        char spec[kMaxString];
        Ihandle ih = 0;
        if (read_string(arg(0), spec, sizeof spec)) {
            // Arguments of the last component: after its first ':'.
            const char* last = spec;
            for (const char* p = spec; *p; ++p)
                if (*p == '/')
                    last = p + 1;
            const char* args = "";
            for (const char* p = last; *p; ++p)
                if (*p == ':') {
                    args = p + 1;
                    break;
                }
            const Phandle ph = tree_.find(spec);
            if (ph != kNone)
                ih = t.open(ph, args);
        }
        ret(0, ih);
        return true;
    }
    if (cstr::equal(s, "close")) {  // ( ihandle -- )
        if (nargs_ < 1)
            return false;
        t.close(arg(0));
        return true;
    }
    if (cstr::equal(s, "read") || cstr::equal(s, "write")) {  // ( ihandle addr len -- actual )
        if (nargs_ < 3 || nrets_ < 1)
            return false;
        PackageDriver* d = t.driver(arg(0));
        if (!d) {
            ret(0, kFail);
            return true;
        }
        const bool is_read = s[0] == 'r';
        const u32 slot = t.slot(arg(0));
        u32 addr = arg(1), left = arg(2), total = 0;
        bool error = false;
        while (left > 0) {
            const u32 chunk = left < sizeof xfer_ ? left : static_cast<u32>(sizeof xfer_);
            s32 n;
            if (is_read) {
                n = d->read(slot, xfer_, chunk);
                if (n > 0 && !write_bytes(addr, xfer_, static_cast<u32>(n)))
                    n = -1;
            } else {
                bool ok = true;
                for (u32 i = 0; ok && i < chunk; ++i)
                    ok = mem_.read8(addr + i, xfer_[i]);
                n = ok ? d->write(slot, xfer_, chunk) : -1;
            }
            if (n < 0) {
                error = total == 0;
                break;
            }
            total += static_cast<u32>(n);
            addr += static_cast<u32>(n);
            left -= static_cast<u32>(n);
            if (static_cast<u32>(n) < chunk)
                break;  // short transfer: end of data
        }
        ret(0, error ? kFail : total);
        return true;
    }
    if (cstr::equal(s, "seek")) {  // ( ihandle pos.hi pos.lo -- status )
        if (nargs_ < 3 || nrets_ < 1)
            return false;
        PackageDriver* d = t.driver(arg(0));
        const u64 pos = (static_cast<u64>(arg(1)) << 32) | arg(2);
        ret(0, d && d->seek(t.slot(arg(0)), pos) ? 0 : kFail);
        return true;
    }
    if (cstr::equal(s, "instance-to-package")) {  // ( ihandle -- phandle )
        if (nargs_ < 1 || nrets_ < 1)
            return false;
        const Phandle ph = t.package(arg(0));
        ret(0, ph == kNone ? kFail : ph);
        return true;
    }
    if (cstr::equal(s, "instance-to-path")) {  // ( ihandle buf buflen -- length )
        if (nargs_ < 3 || nrets_ < 1)
            return false;
        if (!t.valid(arg(0))) {
            ret(0, kFail);
            return true;
        }
        char path[kMaxString];
        u32 len = tree_.path_of(t.package(arg(0)), path, sizeof path);
        const char* a = t.args(arg(0));
        if (*a && len + 1 < sizeof path) {
            path[len++] = ':';
            for (; *a && len + 1 < sizeof path; ++a)
                path[len++] = *a;
            path[len] = '\0';
        }
        ret(0, len < sizeof path ? copy_out(arg(1), path, len, arg(2)) : kFail);
        return true;
    }
    return false;
}

bool ClientInterface::run_forth(const char* text, u32 first, u32 method_addr, u32 method_len, u32 ihandle)
{
    forth::Forth& f = *forth_;
    const u32 base_depth = f.depth();
    // Stack arguments: the last one first, so arg1 ends up on top.
    for (u32 i = nargs_; i-- > first;)
        f.push(arg(i));
    if (method_len) {
        f.push(method_addr);
        f.push(method_len);
        f.push(ihandle);
    }
    const bool ok = f.interpret(text, cstr::length(text));
    // catch-result, then results with ret1 = top of the stack.
    ret(0, ok ? 0 : static_cast<u32>(f.error_code()));
    for (u32 i = 1; i < nrets_; ++i)
        ret(i, (ok && f.depth() > base_depth) ? f.pop() : 0);
    while (f.depth() > base_depth)
        f.pop();  // anything left over is discarded
    return true;
}

bool ClientInterface::dispatch_forth()
{
    const char* s = service_;
    if (cstr::equal(s, "interpret")) {
        if (nargs_ < 1 || nrets_ < 1)
            return false;
        if (!read_string(arg(0), command_, sizeof command_)) {
            ret(0, static_cast<u32>(forth::Forth::kInvalidAddress));
            return true;
        }
        return run_forth(command_, 1);
    }
    if (cstr::equal(s, "call-method")) {
        if (nargs_ < 2 || nrets_ < 1)
            return false;
        // The method name stays in client memory; $call-method reads it there.
        char method[64];
        if (!read_string(arg(0), method, sizeof method)) {
            ret(0, static_cast<u32>(forth::Forth::kInvalidAddress));
            return true;
        }
        return run_forth("$call-method", 2, arg(0), cstr::length(method), arg(1));
    }
    return false;
}

bool ClientInterface::dispatch()
{
    if (forth_ && dispatch_forth())
        return true;
    if (memory_ && dispatch_memory())
        return true;
    if (instances_ && dispatch_instances())
        return true;

    const char* s = service_;
    auto is = [s](const char* name) { return cstr::equal(s, name); };
    auto need = [this](u32 n, u32 r) { return nargs_ >= n && nrets_ >= r; };
    char name[kMaxString];

    if (is("test")) {  // ( name -- missing? )
        if (!need(1, 1))
            return false;
        ret(0, read_string(arg(0), name, sizeof name) && provides(name) ? 0 : kFail);
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
    if (is("quiesce")) {  // ( -- ): Apple: stop firmware activity before the OS takes over.
        return true;      // ours has none (no DMA or timers of its own) — docs/firmware/open-firmware.md
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
