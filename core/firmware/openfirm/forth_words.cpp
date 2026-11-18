// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/forth_words.cpp — see forth_words.h.

#include "core/firmware/openfirm/forth_words.h"

#include "core/common/cstring.h"

namespace g3::of {

using forth::Cell;
using forth::Forth;

namespace {
constexpr Cell kTrue = 0xFFFFFFFFu;
constexpr u32 kMaxPath = 256;
constexpr s32 kErrNoPackage = -21;   // "unsupported operation" style code for missing nodes
constexpr s32 kErrBadArgs = -24;     // invalid numeric argument
} // namespace

FirmwareWords::FirmwareWords(Forth& f, DeviceTree& tree, InstanceTable& inst, u32 scratch_base,
                             u32 scratch_size, u32 load_base, u32 load_limit)
    : f_(f), tree_(tree), inst_(inst), scratch_base_(scratch_base), scratch_size_(scratch_size),
      load_base_(load_base), load_limit_(load_limit)
{
    const Def defs[] = {
        {"dev", w_dev}, {"find-device", w_find_device}, {"device-end", w_device_end},
        {"unselect-dev", w_device_end}, {"active-package", w_active_package},
        {"find-package", w_find_package}, {"get-package-property", w_get_package_property},
        {"get-inherited-property", w_get_inherited_property}, {"property", w_property},
        {"new-device", w_new_device}, {"finish-device", w_finish_device},
        {"device-name", w_device_name}, {"device-type", w_device_type},
        {"encode-int", w_encode_int}, {"encode-string", w_encode_string},
        {"encode-bytes", w_encode_bytes}, {"encode+", w_encode_plus},
        {"decode-int", w_decode_int}, {"decode-string", w_decode_string},
        {"open-dev", w_open_dev}, {"close-dev", w_close_dev}, {"$call-method", w_call_method},
        {"load-base", w_load_base}, {"load-size", w_load_size}, {"load", w_load},
        {"ms", w_ms}, {"get-msecs", w_get_msecs}, {"init-program", w_init_program},
        {"go", w_go}, {"boot", w_boot}, {"reg", w_reg}, {"my-self", w_my_self},
        {"ihandle>phandle", w_ihandle_to_phandle},
    };
    for (const Def& d : defs)
        f_.define_native(d.name, d.fn, this);
}

// --- helpers -------------------------------------------------------------------------------

u32 FirmwareWords::scratch(u32 n)
{
    if (n > scratch_size_)
        return 0;
    if (scratch_pos_ + n > scratch_size_)
        scratch_pos_ = 0;  // round-robin
    const u32 a = scratch_base_ + scratch_pos_;
    scratch_pos_ += (n + 3) & ~3u;
    return a;
}

u32 FirmwareWords::copy_out(const u8* data, u32 len)
{
    const u32 a = scratch(len ? len : 1);
    if (!a) {
        f_.fail(-8, "property too large for scratch space");
        return 0;
    }
    ppc::MemoryInterface& m = f_.memory();
    for (u32 i = 0; i < len; ++i)
        m.write8(a + i, data[i]);
    return a;
}

bool FirmwareWords::read_name(u32 addr, u32 len, char* out, u32 max)
{
    if (len >= max) {
        f_.fail(kErrBadArgs, "name too long");
        return false;
    }
    return f_.read_string(addr, len, out, max);
}

void FirmwareWords::push_property(Phandle h, const char* name)
{
    const u8* data = nullptr;
    u32 len = 0;
    if (!tree_.get_property(h, name, data, len)) {
        f_.push(kTrue);
        return;
    }
    const u32 a = copy_out(data, len);
    if (f_.failed())
        return;
    f_.push(a);
    f_.push(len);
    f_.push(0);
}

Ihandle FirmwareWords::open_path(const char* spec)
{
    // Arguments of the last component start after its first ':'.
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
    return ph == kNone ? 0 : inst_.open(ph, args);
}

// --- device tree ----------------------------------------------------------------------------

void FirmwareWords::w_dev(Forth& f, void* ctx)
{
    const char* w;
    u32 len;
    if (!f.next_word(w, len)) {
        f.fail(-16, "dev: path expected");
        return;
    }
    char path[kMaxPath];
    if (len >= sizeof path) {
        f.fail(kErrBadArgs, "path too long");
        return;
    }
    cstr::copy_bytes(path, w, len);
    path[len] = '\0';
    FirmwareWords& s = self(ctx);
    const Phandle h = s.tree_.find(path);
    if (h == kNone) {
        f.fail(kErrNoPackage, "dev: no such device");
        return;
    }
    s.active_ = h;
}

void FirmwareWords::w_find_device(Forth& f, void* ctx)
{
    const Cell len = f.pop(), addr = f.pop();
    char path[kMaxPath];
    FirmwareWords& s = self(ctx);
    if (f.failed() || !s.read_name(addr, len, path, sizeof path))
        return;
    const Phandle h = s.tree_.find(path);
    if (h == kNone) {
        f.fail(kErrNoPackage, "find-device: no such device");
        return;
    }
    s.active_ = h;
}

void FirmwareWords::w_device_end(Forth&, void* ctx) { self(ctx).active_ = kNone; }
void FirmwareWords::w_active_package(Forth& f, void* ctx) { f.push(self(ctx).active_); }

void FirmwareWords::w_find_package(Forth& f, void* ctx)
{
    // Support packages live under /packages; a path finds any node.
    const Cell len = f.pop(), addr = f.pop();
    char name[kMaxPath];
    FirmwareWords& s = self(ctx);
    if (f.failed() || !s.read_name(addr, len, name, sizeof name))
        return;
    Phandle h = kNone;
    if (name[0] == '/') {
        h = s.tree_.find(name);
    } else {
        const Phandle pk = s.tree_.find("/packages");
        for (Phandle c = pk ? s.tree_.child(pk) : kNone; c != kNone; c = s.tree_.peer(c))
            if (cstr::equal(s.tree_.name(c), name)) {
                h = c;
                break;
            }
    }
    if (h == kNone) {
        f.push(0);
    } else {
        f.push(h);
        f.push(kTrue);
    }
}

void FirmwareWords::w_get_package_property(Forth& f, void* ctx)
{
    const Cell ph = f.pop(), len = f.pop(), addr = f.pop();
    char name[DeviceTree::kMaxNameLength + 2];
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (len > DeviceTree::kMaxNameLength) {
        f.push(kTrue);  // can't be a property name
        return;
    }
    if (!s.read_name(addr, len, name, sizeof name))
        return;
    s.push_property(ph, name);
}

void FirmwareWords::w_get_inherited_property(Forth& f, void* ctx)
{
    const Cell len = f.pop(), addr = f.pop();
    char name[DeviceTree::kMaxNameLength + 2];
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (len > DeviceTree::kMaxNameLength) {
        f.push(kTrue);
        return;
    }
    if (!s.read_name(addr, len, name, sizeof name))
        return;
    for (Phandle h = s.active_; h != kNone; h = s.tree_.parent(h))
        if (s.tree_.has_property(h, name)) {
            s.push_property(h, name);
            return;
        }
    f.push(kTrue);
}

void FirmwareWords::w_property(Forth& f, void* ctx)
{
    const Cell nlen = f.pop(), naddr = f.pop(), plen = f.pop(), paddr = f.pop();
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (s.active_ == kNone) {
        f.fail(kErrNoPackage, "property: no active package");
        return;
    }
    char name[DeviceTree::kMaxNameLength + 2];
    if (nlen > DeviceTree::kMaxNameLength) {
        f.fail(kErrBadArgs, "property: name too long");
        return;
    }
    if (!s.read_name(naddr, nlen, name, sizeof name))
        return;
    if (plen > sizeof s.buf_) {
        f.fail(kErrBadArgs, "property: value too large");
        return;
    }
    for (u32 i = 0; i < plen; ++i)
        if (!f.memory().read8(paddr + i, s.buf_[i])) {
            f.fail(Forth::kInvalidAddress, "invalid address");
            return;
        }
    if (!s.tree_.set_property(s.active_, name, s.buf_, plen))
        f.fail(-8, "property: device tree full");
}

void FirmwareWords::w_new_device(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    const Phandle parent = s.active_ == kNone ? s.tree_.root() : s.active_;
    const Phandle h = s.tree_.add_node(parent, "");
    if (h == kNone) {
        f.fail(-8, "new-device: device tree full");
        return;
    }
    if (s.building_depth_ >= sizeof s.building_ / sizeof s.building_[0]) {
        f.fail(-8, "new-device: nested too deeply");
        return;
    }
    s.building_[s.building_depth_++] = h;
    s.active_ = h;
}

void FirmwareWords::w_finish_device(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    if (s.active_ == kNone || s.active_ == s.tree_.root()) {
        f.fail(kErrNoPackage, "finish-device: no device being built");
        return;
    }
    if (s.building_depth_ > 0 && s.building_[s.building_depth_ - 1] == s.active_)
        --s.building_depth_;
    s.active_ = s.tree_.parent(s.active_);
}

void FirmwareWords::w_reg(Forth& f, void* ctx)
{
    const Cell size = f.pop(), phys = f.pop();
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (s.active_ == kNone) {
        f.fail(kErrNoPackage, "reg: no active package");
        return;
    }
    const u32 v[2] = {phys, size};
    if (!s.tree_.set_ints(s.active_, "reg", v, 2))
        f.fail(-8, "reg: device tree full");
}

void FirmwareWords::w_my_self(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    f.push(s.building_depth_ ? kSelfBase + s.building_depth_ - 1 : 0);
}

void FirmwareWords::w_ihandle_to_phandle(Forth& f, void* ctx)
{
    const Cell ih = f.pop();
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (ih >= kSelfBase && ih - kSelfBase < s.building_depth_) {
        f.push(s.building_[ih - kSelfBase]);
        return;
    }
    const Phandle ph = s.inst_.package(ih);
    if (ph == kNone) {
        f.fail(kErrNoPackage, "ihandle>phandle: invalid ihandle");
        return;
    }
    f.push(ph);
}

void FirmwareWords::w_device_name(Forth& f, void* ctx)
{
    const Cell len = f.pop(), addr = f.pop();
    FirmwareWords& s = self(ctx);
    char name[DeviceTree::kMaxNameLength + 2];
    if (f.failed() || !s.read_name(addr, len, name, sizeof name))
        return;
    if (s.active_ == kNone || !s.tree_.set_name(s.active_, name))
        f.fail(kErrNoPackage, "device-name: no active device");
}

void FirmwareWords::w_device_type(Forth& f, void* ctx)
{
    const Cell len = f.pop(), addr = f.pop();
    FirmwareWords& s = self(ctx);
    char type[64];
    if (f.failed() || !s.read_name(addr, len, type, sizeof type))
        return;
    if (s.active_ == kNone || !s.tree_.set_string(s.active_, "device_type", type))
        f.fail(kErrNoPackage, "device-type: no active device");
}

// --- encoding --------------------------------------------------------------------------------

void FirmwareWords::w_encode_int(Forth& f, void* ctx)
{
    const Cell n = f.pop();
    if (f.failed())
        return;
    const u8 b[4] = {static_cast<u8>(n >> 24), static_cast<u8>(n >> 16), static_cast<u8>(n >> 8),
                     static_cast<u8>(n)};
    const u32 a = self(ctx).copy_out(b, 4);
    if (f.failed())
        return;
    f.push(a);
    f.push(4);
}

void FirmwareWords::w_encode_bytes(Forth& f, void* ctx)
{
    const Cell len = f.pop(), addr = f.pop();
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (len > sizeof s.buf_) {
        f.fail(kErrBadArgs, "encode: too long");
        return;
    }
    for (u32 i = 0; i < len; ++i)
        if (!f.memory().read8(addr + i, s.buf_[i])) {
            f.fail(Forth::kInvalidAddress, "invalid address");
            return;
        }
    const u32 a = s.copy_out(s.buf_, len);
    if (f.failed())
        return;
    f.push(a);
    f.push(len);
}

void FirmwareWords::w_encode_string(Forth& f, void* ctx)
{
    // As encode-bytes, plus the terminating NUL.
    const Cell len = f.pop(), addr = f.pop();
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (len + 1 > sizeof s.buf_) {
        f.fail(kErrBadArgs, "encode: too long");
        return;
    }
    for (u32 i = 0; i < len; ++i)
        if (!f.memory().read8(addr + i, s.buf_[i])) {
            f.fail(Forth::kInvalidAddress, "invalid address");
            return;
        }
    s.buf_[len] = 0;
    const u32 a = s.copy_out(s.buf_, len + 1);
    if (f.failed())
        return;
    f.push(a);
    f.push(len + 1);
}

void FirmwareWords::w_encode_plus(Forth& f, void* ctx)
{
    const Cell l2 = f.pop(), a2 = f.pop(), l1 = f.pop(), a1 = f.pop();
    FirmwareWords& s = self(ctx);
    if (f.failed())
        return;
    if (l1 + l2 > sizeof s.buf_ || l1 + l2 < l1) {
        f.fail(kErrBadArgs, "encode+: too long");
        return;
    }
    ppc::MemoryInterface& m = f.memory();
    bool ok = true;
    for (u32 i = 0; ok && i < l1; ++i)
        ok = m.read8(a1 + i, s.buf_[i]);
    for (u32 i = 0; ok && i < l2; ++i)
        ok = m.read8(a2 + i, s.buf_[l1 + i]);
    if (!ok) {
        f.fail(Forth::kInvalidAddress, "invalid address");
        return;
    }
    const u32 a = s.copy_out(s.buf_, l1 + l2);
    if (f.failed())
        return;
    f.push(a);
    f.push(l1 + l2);
}

void FirmwareWords::w_decode_int(Forth& f, void*)
{
    const Cell len = f.pop(), addr = f.pop();
    if (f.failed())
        return;
    if (len < 4) {
        f.fail(kErrBadArgs, "decode-int: property too short");
        return;
    }
    u32 v = 0;
    if (!f.memory().read32(addr, v)) {
        f.fail(Forth::kInvalidAddress, "invalid address");
        return;
    }
    f.push(addr + 4);
    f.push(len - 4);
    f.push(v);
}

void FirmwareWords::w_decode_string(Forth& f, void*)
{
    // The string runs to the first NUL (excluded) or the end of the property.
    const Cell len = f.pop(), addr = f.pop();
    if (f.failed())
        return;
    u32 n = 0;
    for (; n < len; ++n) {
        u8 c = 0;
        if (!f.memory().read8(addr + n, c)) {
            f.fail(Forth::kInvalidAddress, "invalid address");
            return;
        }
        if (c == 0)
            break;
    }
    const u32 used = n < len ? n + 1 : n;  // skip the NUL
    f.push(addr + used);
    f.push(len - used);
    f.push(addr);
    f.push(n);
}

// --- packages ---------------------------------------------------------------------------------

void FirmwareWords::w_open_dev(Forth& f, void* ctx)
{
    const Cell len = f.pop(), addr = f.pop();
    char path[kMaxPath];
    FirmwareWords& s = self(ctx);
    if (f.failed() || !s.read_name(addr, len, path, sizeof path))
        return;
    f.push(s.open_path(path));
}

void FirmwareWords::w_close_dev(Forth& f, void* ctx)
{
    const Cell ih = f.pop();
    if (!f.failed())
        self(ctx).inst_.close(ih);
}

void FirmwareWords::w_call_method(Forth& f, void* ctx)
{
    const Cell ih = f.pop(), mlen = f.pop(), maddr = f.pop();
    FirmwareWords& s = self(ctx);
    char method[32];
    if (f.failed() || !s.read_name(maddr, mlen, method, sizeof method))
        return;
    PackageDriver* d = s.inst_.driver(ih);
    if (!d) {
        f.fail(kErrNoPackage, "$call-method: invalid ihandle");
        return;
    }
    const u32 slot = s.inst_.slot(ih);
    ppc::MemoryInterface& m = f.memory();
    if (cstr::equal(method, "read") || cstr::equal(method, "write")) {  // ( addr len -- actual )
        const Cell len = f.pop(), addr = f.pop();
        if (f.failed())
            return;
        const bool is_read = method[0] == 'r';
        u32 done = 0;
        bool error = false;
        while (done < len) {
            const u32 chunk = len - done < sizeof s.buf_ ? len - done : static_cast<u32>(sizeof s.buf_);
            s32 n;
            if (is_read) {
                n = d->read(slot, s.buf_, chunk);
                for (s32 i = 0; i < n; ++i)
                    m.write8(addr + done + static_cast<u32>(i), s.buf_[i]);
            } else {
                for (u32 i = 0; i < chunk; ++i)
                    m.read8(addr + done + i, s.buf_[i]);
                n = d->write(slot, s.buf_, chunk);
            }
            if (n < 0) {
                error = done == 0;
                break;
            }
            done += static_cast<u32>(n);
            if (static_cast<u32>(n) < chunk)
                break;
        }
        f.push(error ? 0xFFFFFFFFu : done);
        return;
    }
    if (cstr::equal(method, "seek")) {  // ( pos.lo pos.hi -- status )
        const Cell hi = f.pop(), lo = f.pop();
        if (f.failed())
            return;
        f.push(d->seek(slot, (static_cast<u64>(hi) << 32) | lo) ? 0 : 0xFFFFFFFFu);
        return;
    }
    if (d->method(slot, method, f))
        return;
    f.fail(-21, "$call-method: method not supported");
}

// --- loading -----------------------------------------------------------------------------------

void FirmwareWords::w_load_base(Forth& f, void* ctx) { f.push(self(ctx).load_base_); }
void FirmwareWords::w_load_size(Forth& f, void* ctx) { f.push(self(ctx).load_size_); }

bool FirmwareWords::load_file(const char* path)
{
    const Ihandle ih = open_path(path);
    if (!ih) {
        f_.fail(kErrNoPackage, "load: can't open");
        return false;
    }
    PackageDriver* d = inst_.driver(ih);
    const u32 slot = inst_.slot(ih);
    ppc::MemoryInterface& m = f_.memory();
    u32 total = 0;
    bool ok = true;
    for (;;) {
        const s32 n = d->read(slot, buf_, sizeof buf_);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (total + static_cast<u32>(n) > load_limit_ - load_base_) {
            ok = false;  // doesn't fit below the load limit
            break;
        }
        for (s32 i = 0; i < n && ok; ++i)
            ok = m.write8(load_base_ + total + static_cast<u32>(i), buf_[i]);
        total += static_cast<u32>(n);
    }
    inst_.close(ih);
    if (!ok) {
        f_.fail(-37, "load: read error or file too large");  // -37: file I/O exception
        return false;
    }
    load_size_ = total;
    program_ready_ = false;
    u32 n = 0;
    for (; path[n] && n + 1 < sizeof load_path_; ++n)
        load_path_[n] = path[n];
    load_path_[n] = '\0';
    return true;
}

namespace {
bool line_path(Forth& f, char* path, u32 max)
{
    const char* text;
    u32 len;
    if (!f.rest_of_line(text, len)) {
        f.fail(-16, "device-specifier expected");
        return false;
    }
    if (len >= max) {
        f.fail(kErrBadArgs, "path too long");
        return false;
    }
    cstr::copy_bytes(path, text, len);
    path[len] = '\0';
    return true;
}
} // namespace

void FirmwareWords::w_load(Forth& f, void* ctx)
{
    char path[kMaxPath];
    if (line_path(f, path, sizeof path))
        self(ctx).load_file(path);
}

// The bootinfo object's own path, split into the entities of CHRP
// 3.1.6.1 ("&device;:[&partition;][,]\&directory;\&filename;"). After the
// partition and comma, the rest splits at its last backslash: before it
// (without a leading backslash) is the directory, after it the filename.
// So "hd:9,\System Folder\Mac OS ROM" gives "hd", "9", "System Folder",
// "Mac OS ROM", and "hd:9,\\:tbxi" gives "hd", "9", "", ":tbxi".
// (How Apple's firmware fills them for "\\:tbxi" paths: to verify.)
void FirmwareWords::run_bootinfo()
{
    char dev[kMaxPath] = {}, part[32] = {}, dir[kMaxPath] = {}, file[kMaxPath] = {};
    auto copy = [](char* out, u32 max, const char* from, const char* to) {
        u32 n = 0;
        for (const char* q = from; q < to && n + 1 < max; ++q)
            out[n++] = *q;
        out[n] = '\0';
    };
    const char* p = load_path_;
    const char* end = p + cstr::length(p);
    const char* colon = p;
    while (colon < end && *colon != ':')
        ++colon;
    copy(dev, sizeof dev, p, colon);
    if (colon < end) {
        const char* q = colon + 1;
        const char* digits = q;
        while (q < end && *q >= '0' && *q <= '9')
            ++q;
        copy(part, sizeof part, digits, q);
        if (q < end && *q == ',')
            ++q;
        const char* last = nullptr;
        for (const char* r = q; r < end; ++r)
            if (*r == '\\')
                last = r;
        if (last) {
            copy(dir, sizeof dir, (*q == '\\' && q < last) ? q + 1 : q, last);
            copy(file, sizeof file, last + 1, end);
        } else {
            copy(file, sizeof file, q, end);
        }
    }
    BootInfoPath path;
    path.device = dev;
    path.partition = part;
    path.directory = dir;
    path.filename = file;
    // Parse from a host copy of the start of the image (the text part).
    const u32 n = load_size_ < sizeof text_ ? load_size_ : static_cast<u32>(sizeof text_);
    for (u32 i = 0; i < n; ++i)
        f_.memory().read8(load_base_ + i, text_[i]);
    if (!parse_bootinfo(text_, n, path, bootinfo_)) {
        f_.fail(-21, "init-program: malformed bootinfo object");
        return;
    }
    if (!bootinfo_.has_script) {
        f_.fail(-21, "init-program: bootinfo object has no boot script");
        return;
    }
    in_script_ = true;
    f_.evaluate(bootinfo_.script, bootinfo_.script_len);
    in_script_ = false;
}

void FirmwareWords::w_init_program(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    s.program_ready_ = false;
    s.ran_script_ = false;
    // Is it a bootinfo object? Look at the start of the image.
    u8 head[64];
    const u32 n = s.load_size_ < sizeof head ? s.load_size_ : static_cast<u32>(sizeof head);
    for (u32 i = 0; i < n; ++i)
        f.memory().read8(s.load_base_ + i, head[i]);
    if (n && is_bootinfo(head, n)) {
        if (s.in_script_) {
            f.fail(-21, "init-program: bootinfo inside a boot script");
            return;
        }
        s.run_bootinfo();
        // Set afterwards: the script's own init-program clears it.
        s.ran_script_ = true;  // the script decided what runs (it may load + go itself)
        return;
    }
    const ElfError e = load_elf_program(f.memory(), s.memory_, s.load_base_, s.load_size_, s.program_);
    if (e != ElfError::None) {
        f.fail(-21, e == ElfError::NotElf ? "init-program: unknown image format"
                                          : "init-program: bad ELF image");
        return;
    }
    s.program_ready_ = true;
}

void FirmwareWords::w_go(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    if (!s.program_ready_) {
        f.fail(-21, "go: no program prepared");
        return;
    }
    if (!s.go_) {
        f.fail(-21, "go: no way to start a program");
        return;
    }
    s.go_(s.go_ctx_, s.program_.entry);
}

void FirmwareWords::w_boot(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    char path[kMaxPath];
    if (!line_path(f, path, sizeof path) || !s.load_file(path))
        return;
    w_init_program(f, ctx);
    if (f.failed() || s.ran_script_ || !s.program_ready_)
        return;  // an error, or a boot script that has run (and may have started a program)
    w_go(f, ctx);
}

void FirmwareWords::w_ms(Forth& f, void*) { f.pop(); }  // time passes on its own
void FirmwareWords::w_get_msecs(Forth& f, void* ctx)
{
    FirmwareWords& s = self(ctx);
    f.push(s.clock_ ? s.clock_(s.clock_ctx_) : 0);
}

} // namespace g3::of
