// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/device_tree.h — the Open Firmware device tree
// that our high-level firmware presents to Mac OS.
//
// Facts (docs/firmware/open-firmware.md; IEEE 1275-1994 and its PowerPC
// binding):
//  - A tree of device nodes ("packages"); each has a "name" property and
//    any number of other properties (name -> byte array). Integers in
//    property values are big-endian 32-bit cells; strings include their NUL.
//  - Device paths: "/node-name@unit-address:arguments/...". Without a unit
//    address the first child with that name matches. A path not starting
//    with '/' starts with an alias, looked up in the /aliases node.
//  - phandles are opaque, non-zero cells; 0 means "none" for peer/child/
//    parent and -1 is the failure value of the client interface.
//
// Simplification: each node stores the text of its unit address (as
// encode-unit of its parent would produce it) instead of deriving it
// from "reg" at lookup time. Unit addresses compare as comma-separated
// hexadecimal numbers, case-insensitive, ignoring leading zeros.
//
// Storage is fixed-size (the core does not allocate). Exhausting it makes
// the failing call return false/kNone and clears ok().

#ifndef G3_CORE_FIRMWARE_OPENFIRM_DEVICE_TREE_H
#define G3_CORE_FIRMWARE_OPENFIRM_DEVICE_TREE_H

#include "core/common/types.h"

namespace g3::of {

using Phandle = u32;
constexpr Phandle kNone = 0;

class DeviceTree {
public:
    static constexpr u32 kMaxNodes = 256;
    static constexpr u32 kMaxProperties = 2048;
    static constexpr u32 kPoolSize = 96 * 1024;
    static constexpr u32 kMaxNameLength = 31;  // property names (nextprop's 32-byte buffer)

    DeviceTree();

    Phandle root() const { return to_handle(0); }
    bool valid(Phandle h) const { return h >= kHandleBase && h - kHandleBase < node_count_; }
    bool ok() const { return ok_; }

    // Builds a node; also sets its "name" property. unit may be nullptr.
    Phandle add_node(Phandle parent, const char* name, const char* unit = nullptr);

    // Properties (replacing an existing one of the same name).
    bool set_property(Phandle h, const char* name, const u8* data, u32 len);
    bool set_string(Phandle h, const char* name, const char* value);  // stored with its NUL
    bool set_int(Phandle h, const char* name, u32 value);
    bool set_ints(Phandle h, const char* name, const u32* values, u32 count);
    // Returns false if absent; data/len point into the tree's storage.
    bool get_property(Phandle h, const char* name, const u8*& data, u32& len) const;
    bool has_property(Phandle h, const char* name) const;

    // Property names in definition order. prev nullptr or "" -> first.
    // Returns nullptr at the end; sets *prev_missing if prev isn't a property.
    const char* next_property(Phandle h, const char* prev, bool* prev_missing = nullptr) const;

    // Tree walking (kNone when there is no such node).
    Phandle parent(Phandle h) const;
    Phandle child(Phandle h) const;
    Phandle peer(Phandle h) const;
    const char* name(Phandle h) const;
    const char* unit(Phandle h) const;  // "" if none

    // Path resolution, with aliases. Returns kNone if not found.
    Phandle find(const char* path) const;
    // Writes the full path ("/a@1/b"), NUL-terminated if it fits.
    // Returns the path length without the NUL (even when truncated).
    u32 path_of(Phandle h, char* buf, u32 buflen) const;

    u32 node_count() const { return node_count_; }

private:
    static constexpr u32 kHandleBase = 0x10000000u;  // arbitrary non-zero base
    static constexpr u32 kNil = 0xFFFFFFFFu;

    struct Node {
        u32 name;       // pool offset of the node name
        u32 unit;       // pool offset of the unit-address text
        u32 parent, first_child, last_child, next_sibling;  // node indices or kNil
        u32 first_prop, last_prop;                          // property indices or kNil
    };
    struct Property {
        u32 name;       // pool offset
        u32 value;      // pool offset
        u32 length;
        u32 next;       // property index or kNil
    };

    static Phandle to_handle(u32 index) { return kHandleBase + index; }
    u32 index(Phandle h) const { return h - kHandleBase; }
    const char* str(u32 off) const { return reinterpret_cast<const char*>(pool_ + off); }
    u32 store(const void* data, u32 len);   // returns pool offset or kNil
    u32 store_string(const char* s, u32 len);
    u32 find_property(u32 node, const char* name) const;
    u32 find_child(u32 node, const char* name, u32 name_len, const char* unit, u32 unit_len) const;
    Phandle resolve(const char* path, u32 depth) const;
    u32 path_into(u32 node, char* buf, u32 buflen, u32 pos) const;

    Node nodes_[kMaxNodes];
    Property props_[kMaxProperties];
    u8 pool_[kPoolSize];
    u32 node_count_ = 0;
    u32 prop_count_ = 0;
    u32 pool_used_ = 0;
    bool ok_ = true;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_DEVICE_TREE_H
