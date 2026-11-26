// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/bootinfo.cpp — see bootinfo.h.

#include "core/firmware/openfirm/bootinfo.h"

#include "core/common/cstring.h"

namespace g3::of {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_space(u8 c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
bool is_name_char(u8 c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

// Case-insensitive compare of data[pos..] with the NUL-terminated word.
bool match_ci(const u8* d, u32 len, u32 pos, const char* word)
{
    for (u32 i = 0; word[i]; ++i)
        if (pos + i >= len || lower(static_cast<char>(d[pos + i])) != word[i])
            return false;
    return true;
}

struct Tag {
    u32 name = 0, name_len = 0;    // name position in the data
    u32 attrs = 0, attrs_len = 0;  // text between the name and '>'
    u32 after = 0;                 // first byte after '>'
    bool end = false;
};

// Parses a tag starting at d[pos] == '<'. False if malformed.
bool read_tag(const u8* d, u32 len, u32 pos, Tag& t)
{
    u32 p = pos + 1;
    t.end = p < len && d[p] == '/';
    if (t.end)
        ++p;
    t.name = p;
    while (p < len && is_name_char(d[p]))
        ++p;
    t.name_len = p - t.name;
    if (t.name_len == 0)
        return false;
    t.attrs = p;
    while (p < len && d[p] != '>')
        ++p;
    if (p >= len)
        return false;
    t.attrs_len = p - t.attrs;
    t.after = p + 1;
    return true;
}

bool tag_is(const u8* d, const Tag& t, const char* name)
{
    return cstr::length(name) == t.name_len && match_ci(d, t.name + t.name_len, t.name, name);
}

// Finds the end tag </name> at or after pos; returns its '<' position.
bool find_end(const u8* d, u32 len, u32 pos, const u8* name, u32 name_len, u32& at, u32& after)
{
    for (u32 p = pos; p + 2 + name_len < len; ++p) {
        if (d[p] != '<' || d[p + 1] != '/')
            continue;
        bool ok = true;
        for (u32 i = 0; ok && i < name_len; ++i)
            ok = lower(static_cast<char>(d[p + 2 + i])) == lower(static_cast<char>(name[i]));
        if (!ok || is_name_char(d[p + 2 + name_len]))
            continue;
        u32 q = p + 2 + name_len;
        while (q < len && d[q] != '>')
            ++q;
        if (q >= len)
            return false;
        at = p;
        after = q + 1;
        return true;
    }
    return false;
}

// Output buffer with overflow tracking.
struct Out {
    char* buf;
    u32 cap, len = 0;
    bool overflow = false;
    void put(char c)
    {
        if (len + 1 < cap)
            buf[len++] = c;
        else
            overflow = true;
    }
    void put(const char* s)
    {
        while (*s)
            put(*s++);
    }
    void finish() { buf[len < cap ? len : cap - 1] = '\0'; }
};

// Copies d[from, to) with entities replaced. Path entities only if `path`.
void expand(const u8* d, u32 from, u32 to, const BootInfoPath* path, Out& out)
{
    for (u32 p = from; p < to; ++p) {
        if (d[p] == '&') {
            u32 q = p + 1;
            while (q < to && q - p <= 16 && is_name_char(d[q]))
                ++q;
            if (q < to && d[q] == ';' && q > p + 1) {
                char name[17];
                u32 n = 0;
                for (u32 i = p + 1; i < q; ++i)
                    name[n++] = lower(static_cast<char>(d[i]));
                name[n] = '\0';
                bool done = true;
                if (cstr::equal(name, "lt")) {
                    out.put('<');
                } else if (cstr::equal(name, "gt")) {
                    out.put('>');
                } else if (cstr::equal(name, "amp")) {
                    out.put('&');
                } else if (path && cstr::equal(name, "device")) {
                    out.put(path->device);
                } else if (path && cstr::equal(name, "partition")) {
                    out.put(path->partition);
                } else if (path && cstr::equal(name, "directory")) {
                    out.put(path->directory);
                } else if (path && cstr::equal(name, "filename")) {
                    out.put(path->filename);
                } else if (path && cstr::equal(name, "full-path")) {
                    // &device;:[&partition;][,]\&directory;\&filename;
                    out.put(path->device);
                    out.put(':');
                    if (path->partition[0]) {
                        out.put(path->partition);
                        out.put(',');
                    }
                    out.put('\\');
                    out.put(path->directory);
                    out.put('\\');
                    out.put(path->filename);
                } else {
                    done = false;  // unknown entity: kept as written
                }
                if (done) {
                    p = q;
                    continue;
                }
            }
        }
        out.put(static_cast<char>(d[p]));
    }
}

// Reads "N,M[,K]" decimal numbers from attribute text after `key=`.
u32 read_numbers(const u8* d, u32 from, u32 to, const char* key, u32* vals, u32 max)
{
    const u32 klen = cstr::length(key);
    for (u32 p = from; p + klen < to; ++p) {
        if (!match_ci(d, to, p, key) || (p > from && is_name_char(d[p - 1])))
            continue;
        u32 q = p + klen;
        while (q < to && is_space(d[q]))
            ++q;
        if (q >= to || d[q] != '=')
            continue;
        ++q;
        u32 n = 0;
        while (n < max) {
            while (q < to && is_space(d[q]))
                ++q;
            if (q >= to || d[q] < '0' || d[q] > '9')
                break;
            u32 v = 0;
            while (q < to && d[q] >= '0' && d[q] <= '9' && v < 100000)
                v = v * 10 + static_cast<u32>(d[q++] - '0');
            vals[n++] = v;
            while (q < to && is_space(d[q]))
                ++q;
            if (q < to && d[q] == ',')
                ++q;
            else
                break;
        }
        return n;
    }
    return 0;
}

int hex(u8 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = static_cast<u8>(lower(static_cast<char>(c)));
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

} // namespace

bool is_bootinfo(const u8* data, u32 len)
{
    u32 p = 0;
    while (p < len && is_space(data[p]))
        ++p;
    return match_ci(data, len, p, "<chrp-boot");
}

bool parse_bootinfo(const u8* d, u32 len, const BootInfoPath& path, BootInfo& out, u32 index)
{
    out = BootInfo{};
    // Find the opening tag of section `index`.
    u32 p = 0, section = 0;
    Tag t;
    for (;;) {
        while (p < len && d[p] != '<')
            ++p;
        if (p >= len)
            return false;
        if (read_tag(d, len, p, t) && !t.end && tag_is(d, t, "chrp-boot")) {
            if (section == index)
                break;
            ++section;
        }
        ++p;
    }
    p = t.after;
    for (;;) {
        while (p < len && d[p] != '<')
            ++p;  // text between elements is ignored
        if (p >= len || !read_tag(d, len, p, t))
            return false;  // no </CHRP-BOOT>
        if (t.end) {
            if (!tag_is(d, t, "chrp-boot"))
                return false;  // stray end tag
            out.body_offset = t.after;
            return true;
        }
        u32 end_at = 0, end_after = 0;
        if (!find_end(d, len, t.after, d + t.name, t.name_len, end_at, end_after))
            return false;
        if (tag_is(d, t, "description") || tag_is(d, t, "os-name")) {
            Out o{tag_is(d, t, "description") ? out.description : out.os_name, BootInfo::kMaxText};
            expand(d, t.after, end_at, nullptr, o);
            o.finish();
            if (o.overflow)
                return false;
        } else if (tag_is(d, t, "boot-script")) {
            Out o{out.script, BootInfo::kMaxScript};
            expand(d, t.after, end_at, &path, o);
            o.finish();
            if (o.overflow)
                return false;
            out.script_len = o.len;
            out.has_script = true;
        } else if (tag_is(d, t, "icon")) {
            out.has_icon = true;
            u32 v[3];
            if (read_numbers(d, t.attrs, t.attrs + t.attrs_len, "size", v, 2) == 2) {
                out.icon_width = v[0];
                out.icon_height = v[1];
            }
            if (read_numbers(d, t.attrs, t.attrs + t.attrs_len, "color-space", v, 3) == 3)
                for (u32 i = 0; i < 3; ++i)
                    out.icon_bits[i] = v[i];
            // <BITMAP> inside the icon.
            for (u32 q = t.after; q < end_at; ++q) {
                Tag b;
                if (d[q] != '<' || !read_tag(d, end_at, q, b) || b.end || !tag_is(d, b, "bitmap"))
                    continue;
                u32 b_at = 0, b_after = 0;
                if (!find_end(d, end_at, b.after, d + b.name, b.name_len, b_at, b_after))
                    return false;
                const u64 want = static_cast<u64>(out.icon_width) * out.icon_height;
                for (u32 h = b.after; h + 1 < b_at && out.icon_pixels < want;) {
                    if (is_space(d[h])) {
                        ++h;
                        continue;
                    }
                    const int hi = hex(d[h]), lo = hex(d[h + 1]);
                    if (hi < 0 || lo < 0)
                        return false;
                    if (out.icon_pixels >= BootInfo::kMaxIconPixels)
                        return false;
                    out.icon[out.icon_pixels++] = static_cast<u8>((hi << 4) | lo);
                    h += 2;
                }
                break;
            }
        }
        // Unknown elements: ignored up to their end tag.
        p = end_after;
    }
}

} // namespace g3::of
