// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/mem/flat_memory.h — one contiguous block of guest memory.
//
// Stores bytes in guest (big-endian) order, as the architecture requires
// (docs/02-architecture.md 3.2). Used by CPU tests now; the real machine
// will use the page-mapped physical bus of Milestone 3.

#ifndef G3_CORE_MEM_FLAT_MEMORY_H
#define G3_CORE_MEM_FLAT_MEMORY_H

#include "core/common/endian.h"
#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"

namespace g3 {

class FlatMemory final : public ppc::MemoryInterface {
public:
    // 'data' must stay valid for the lifetime of this object.
    FlatMemory(u8* data, u32 size, u32 base = 0) : data_(data), size_(size), base_(base) {}

    bool read8(u32 a, u8& v) override
    {
        if (!in_range(a, 1)) return false;
        v = data_[a - base_];
        return true;
    }
    bool read16(u32 a, u16& v) override
    {
        if (!in_range(a, 2)) return false;
        v = load_be16(data_ + (a - base_));
        return true;
    }
    bool read32(u32 a, u32& v) override
    {
        if (!in_range(a, 4)) return false;
        v = load_be32(data_ + (a - base_));
        return true;
    }
    bool write8(u32 a, u8 v) override
    {
        if (!in_range(a, 1)) return false;
        data_[a - base_] = v;
        return true;
    }
    bool write16(u32 a, u16 v) override
    {
        if (!in_range(a, 2)) return false;
        store_be16(data_ + (a - base_), v);
        return true;
    }
    bool write32(u32 a, u32 v) override
    {
        if (!in_range(a, 4)) return false;
        store_be32(data_ + (a - base_), v);
        return true;
    }

    u8* direct_window(u32& base, u32& size) override
    {
        base = base_;
        size = size_;
        return data_;
    }

    u8* data() { return data_; }
    u32 size() const { return size_; }
    u32 base() const { return base_; }

private:
    bool in_range(u32 a, u32 len) const
    {
        // Written to avoid overflow for addresses near 0xFFFFFFFF.
        return a >= base_ && (a - base_) <= size_ && len <= size_ - (a - base_);
    }

    u8* data_;
    u32 size_;
    u32 base_;
};

} // namespace g3

#endif // G3_CORE_MEM_FLAT_MEMORY_H
