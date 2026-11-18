// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/device_tree.cpp — see device_tree.h.

#include "core/firmware/openfirm/device_tree.h"

#include "core/common/cstring.h"

namespace g3::of {

namespace {

constexpr u32 kMaxPathDepth = 32;

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Unit addresses: comma-separated hex numbers compare numerically
// ("0010" == "10", "1F" == "1f"); anything else compares as text,
// ignoring case.
bool unit_equal(const char* a, u32 alen, const char* b, u32 blen)
{
    u32 i = 0, j = 0;
    bool numeric = true;
    while (numeric && (i < alen || j < blen)) {
        u64 va = 0, vb = 0;
        u32 da = 0, db = 0;
        while (i < alen && a[i] != ',') {
            const int d = hex_digit(a[i++]);
            if (d < 0 || va > (~0ull >> 4)) { numeric = false; break; }
            va = (va << 4) | static_cast<u64>(d);
            ++da;
        }
        while (numeric && j < blen && b[j] != ',') {
            const int d = hex_digit(b[j++]);
            if (d < 0 || vb > (~0ull >> 4)) { numeric = false; break; }
            vb = (vb << 4) | static_cast<u64>(d);
            ++db;
        }
        if (!numeric)
            break;
        if (da == 0 || db == 0 || va != vb)
            return false;
        const bool ca = i < alen, cb = j < blen;  // a ',' follows
        if (ca != cb)
            return false;
        if (ca) { ++i; ++j; }
    }
    if (numeric)
        return true;
    if (alen != blen)
        return false;
    for (u32 k = 0; k < alen; ++k)
        if (lower(a[k]) != lower(b[k]))
            return false;
    return true;
}

} // namespace

DeviceTree::DeviceTree()
{
    pool_[0] = 0;  // offset 0 = the empty string
    pool_used_ = 1;
    Node& r = nodes_[0];
    r.name = 0;
    r.unit = 0;
    r.parent = r.first_child = r.last_child = r.next_sibling = kNil;
    r.first_prop = r.last_prop = kNil;
    node_count_ = 1;
}

u32 DeviceTree::store(const void* data, u32 len)
{
    if (len > kPoolSize - pool_used_) {
        ok_ = false;
        return kNil;
    }
    const u32 off = pool_used_;
    if (len)
        cstr::copy_bytes(pool_ + off, data, len);
    pool_used_ += len;
    return off;
}

u32 DeviceTree::store_string(const char* s, u32 len)
{
    if (len + 1 > kPoolSize - pool_used_) {
        ok_ = false;
        return kNil;
    }
    const u32 off = store(s, len);
    pool_[pool_used_++] = 0;
    return off;
}

Phandle DeviceTree::add_node(Phandle parent_h, const char* name, const char* unit)
{
    if (!valid(parent_h) || node_count_ >= kMaxNodes) {
        ok_ = false;
        return kNone;
    }
    const u32 name_off = store_string(name, cstr::length(name));
    const u32 unit_off = unit ? store_string(unit, cstr::length(unit)) : 0;
    if (name_off == kNil || unit_off == kNil)
        return kNone;
    const u32 p = index(parent_h);
    const u32 n = node_count_++;
    Node& node = nodes_[n];
    node.name = name_off;
    node.unit = unit_off;
    node.parent = p;
    node.first_child = node.last_child = node.next_sibling = kNil;
    node.first_prop = node.last_prop = kNil;
    if (nodes_[p].last_child == kNil)
        nodes_[p].first_child = n;
    else
        nodes_[nodes_[p].last_child].next_sibling = n;
    nodes_[p].last_child = n;
    const Phandle h = to_handle(n);
    if (!set_string(h, "name", name))
        return kNone;
    return h;
}

bool DeviceTree::set_name(Phandle h, const char* name, const char* unit)
{
    if (!valid(h) || index(h) == 0)
        return false;  // the root has no name of its own
    const u32 name_off = store_string(name, cstr::length(name));
    if (name_off == kNil)
        return false;
    u32 unit_off = nodes_[index(h)].unit;
    if (unit) {
        unit_off = store_string(unit, cstr::length(unit));
        if (unit_off == kNil)
            return false;
    }
    nodes_[index(h)].name = name_off;
    nodes_[index(h)].unit = unit_off;
    return set_string(h, "name", name);
}

u32 DeviceTree::find_property(u32 node, const char* name) const
{
    for (u32 p = nodes_[node].first_prop; p != kNil; p = props_[p].next)
        if (cstr::equal(str(props_[p].name), name))
            return p;
    return kNil;
}

bool DeviceTree::set_property(Phandle h, const char* name, const u8* data, u32 len)
{
    const u32 name_len = cstr::length(name);
    if (!valid(h) || name_len == 0 || name_len > kMaxNameLength)
        return false;
    const u32 n = index(h);
    u32 p = find_property(n, name);
    const u32 value_off = store(data, len);
    if (value_off == kNil)
        return false;
    if (p == kNil) {
        if (prop_count_ >= kMaxProperties) {
            ok_ = false;
            return false;
        }
        const u32 name_off = store_string(name, name_len);
        if (name_off == kNil)
            return false;
        p = prop_count_++;
        props_[p].name = name_off;
        props_[p].next = kNil;
        if (nodes_[n].last_prop == kNil)
            nodes_[n].first_prop = p;
        else
            props_[nodes_[n].last_prop].next = p;
        nodes_[n].last_prop = p;
    }
    props_[p].value = value_off;  // a replaced value's old bytes stay unused
    props_[p].length = len;
    return true;
}

bool DeviceTree::set_string(Phandle h, const char* name, const char* value)
{
    return set_property(h, name, reinterpret_cast<const u8*>(value), cstr::length(value) + 1);
}

bool DeviceTree::set_int(Phandle h, const char* name, u32 value) { return set_ints(h, name, &value, 1); }

bool DeviceTree::set_ints(Phandle h, const char* name, const u32* values, u32 count)
{
    u8 buf[256];
    if (count > sizeof buf / 4)
        return false;
    for (u32 i = 0; i < count; ++i) {
        buf[4 * i + 0] = static_cast<u8>(values[i] >> 24);
        buf[4 * i + 1] = static_cast<u8>(values[i] >> 16);
        buf[4 * i + 2] = static_cast<u8>(values[i] >> 8);
        buf[4 * i + 3] = static_cast<u8>(values[i]);
    }
    return set_property(h, name, buf, count * 4);
}

bool DeviceTree::get_property(Phandle h, const char* name, const u8*& data, u32& len) const
{
    if (!valid(h))
        return false;
    const u32 p = find_property(index(h), name);
    if (p == kNil)
        return false;
    data = pool_ + props_[p].value;
    len = props_[p].length;
    return true;
}

bool DeviceTree::has_property(Phandle h, const char* name) const
{
    return valid(h) && find_property(index(h), name) != kNil;
}

const char* DeviceTree::next_property(Phandle h, const char* prev, bool* prev_missing) const
{
    if (prev_missing)
        *prev_missing = false;
    if (!valid(h))
        return nullptr;
    const u32 n = index(h);
    u32 p;
    if (prev == nullptr || prev[0] == '\0') {
        p = nodes_[n].first_prop;
    } else {
        const u32 cur = find_property(n, prev);
        if (cur == kNil) {
            if (prev_missing)
                *prev_missing = true;
            return nullptr;
        }
        p = props_[cur].next;
    }
    return p == kNil ? nullptr : str(props_[p].name);
}

Phandle DeviceTree::parent(Phandle h) const
{
    if (!valid(h) || nodes_[index(h)].parent == kNil)
        return kNone;
    return to_handle(nodes_[index(h)].parent);
}

Phandle DeviceTree::child(Phandle h) const
{
    if (!valid(h) || nodes_[index(h)].first_child == kNil)
        return kNone;
    return to_handle(nodes_[index(h)].first_child);
}

Phandle DeviceTree::peer(Phandle h) const
{
    if (h == kNone)
        return root();  // IEEE 1275: peer(0) is the root node
    if (!valid(h) || nodes_[index(h)].next_sibling == kNil)
        return kNone;
    return to_handle(nodes_[index(h)].next_sibling);
}

const char* DeviceTree::name(Phandle h) const { return valid(h) ? str(nodes_[index(h)].name) : ""; }
const char* DeviceTree::unit(Phandle h) const { return valid(h) ? str(nodes_[index(h)].unit) : ""; }

// --- paths ----------------------------------------------------------------------------

u32 DeviceTree::find_child(u32 node, const char* name, u32 name_len, const char* unit,
                           u32 unit_len) const
{
    // "@unit" alone (no node name) matches any child at that unit address
    // (IEEE 1275 path resolution; Apple's Mac OS ROM loader looks up "/cpus/@0").
    if (name_len == 0 && unit == nullptr)
        return kNil;
    for (u32 c = nodes_[node].first_child; c != kNil; c = nodes_[c].next_sibling) {
        if (name_len != 0 && !cstr::equal_n(name, name_len, str(nodes_[c].name)))
            continue;
        if (unit == nullptr)
            return c;  // no unit address in the path: first match by name
        const char* u = str(nodes_[c].unit);
        if (unit_equal(unit, unit_len, u, cstr::length(u)))
            return c;
    }
    return kNil;
}

Phandle DeviceTree::find(const char* path) const { return resolve(path, 0); }

Phandle DeviceTree::resolve(const char* path, u32 depth) const
{
    if (path == nullptr || path[0] == '\0' || depth > 1)
        return kNone;
    if (path[0] != '/') {
        // Alias: the first component (up to '/' or ':') names a property
        // of /aliases holding a path; the rest of the path follows it.
        u32 n = 0;
        while (path[n] != '\0' && path[n] != '/' && path[n] != ':')
            ++n;
        const u32 aliases = find_child(0, "aliases", 7, nullptr, 0);
        if (aliases == kNil || n == 0 || n > kMaxNameLength)
            return kNone;
        char alias[kMaxNameLength + 1];
        cstr::copy_bytes(alias, path, n);
        alias[n] = '\0';
        const u8* value = nullptr;
        u32 len = 0;
        if (!get_property(to_handle(aliases), alias, value, len) || len == 0)
            return kNone;
        char full[256];
        u32 vlen = len;
        while (vlen > 0 && value[vlen - 1] == 0)
            --vlen;
        const u32 rest = cstr::length(path + n);
        if (vlen + rest + 1 > sizeof full)
            return kNone;
        cstr::copy_bytes(full, value, vlen);
        cstr::copy_bytes(full + vlen, path + n, rest + 1);
        return resolve(full, depth + 1);
    }
    u32 node = 0;
    const char* p = path;
    for (u32 level = 0; *p != '\0'; ++level) {
        while (*p == '/')
            ++p;
        if (*p == '\0')
            break;
        if (level >= kMaxPathDepth)
            return kNone;
        const char* comp = p;
        while (*p != '\0' && *p != '/')
            ++p;
        const u32 comp_len = static_cast<u32>(p - comp);
        // node-name[@unit-address][:arguments]
        u32 name_len = 0;
        while (name_len < comp_len && comp[name_len] != '@' && comp[name_len] != ':')
            ++name_len;
        const char* unit = nullptr;
        u32 unit_len = 0;
        if (name_len < comp_len && comp[name_len] == '@') {
            unit = comp + name_len + 1;
            while (name_len + 1 + unit_len < comp_len && unit[unit_len] != ':')
                ++unit_len;
        }
        const u32 next = find_child(node, comp, name_len, unit, unit_len);
        if (next == kNil)
            return kNone;
        node = next;
    }
    return to_handle(node);
}

u32 DeviceTree::path_into(u32 node, char* buf, u32 buflen, u32 pos) const
{
    if (node == 0)
        return pos;
    pos = path_into(nodes_[node].parent, buf, buflen, pos);
    auto put = [&](char c) {
        if (pos < buflen)
            buf[pos] = c;
        ++pos;
    };
    put('/');
    for (const char* s = str(nodes_[node].name); *s; ++s)
        put(*s);
    const char* u = str(nodes_[node].unit);
    if (*u) {
        put('@');
        for (; *u; ++u)
            put(*u);
    }
    return pos;
}

u32 DeviceTree::path_of(Phandle h, char* buf, u32 buflen) const
{
    if (!valid(h)) {
        if (buflen)
            buf[0] = '\0';
        return 0;
    }
    u32 len;
    if (index(h) == 0) {
        if (buflen)
            buf[0] = '/';
        len = 1;
    } else {
        len = path_into(index(h), buf, buflen, 0);
    }
    if (len < buflen)
        buf[len] = '\0';
    return len;
}

} // namespace g3::of
