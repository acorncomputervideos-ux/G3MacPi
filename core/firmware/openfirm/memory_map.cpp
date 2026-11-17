// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/memory_map.cpp — see memory_map.h.

#include "core/firmware/openfirm/memory_map.h"

namespace g3::of {

bool MemoryMap::insert(u64 base, u64 end)
{
    if (base >= end)
        return true;
    // Position: first range that starts after base.
    u32 i = 0;
    while (i < count_ && base_[i] <= base)
        ++i;
    // Overlap with the previous or next range means a double free.
    if (i > 0 && end_[i - 1] > base)
        return false;
    if (i < count_ && base_[i] < end)
        return false;
    const bool join_prev = i > 0 && end_[i - 1] == base;
    const bool join_next = i < count_ && base_[i] == end;
    if (join_prev && join_next) {
        end_[i - 1] = end_[i];
        for (u32 k = i; k + 1 < count_; ++k) {
            base_[k] = base_[k + 1];
            end_[k] = end_[k + 1];
        }
        --count_;
        return true;
    }
    if (join_prev) {
        end_[i - 1] = end;
        return true;
    }
    if (join_next) {
        base_[i] = base;
        return true;
    }
    if (count_ >= kMaxRanges)
        return false;
    for (u32 k = count_; k > i; --k) {
        base_[k] = base_[k - 1];
        end_[k] = end_[k - 1];
    }
    base_[i] = base;
    end_[i] = end;
    ++count_;
    return true;
}

bool MemoryMap::add_free(u32 base, u32 size) { return insert(base, static_cast<u64>(base) + size); }

bool MemoryMap::take(u32 i, u64 start, u64 end)
{
    const u64 b = base_[i], e = end_[i];
    if (start == b && end == e) {
        for (u32 k = i; k + 1 < count_; ++k) {
            base_[k] = base_[k + 1];
            end_[k] = end_[k + 1];
        }
        --count_;
        return true;
    }
    if (start == b) {
        base_[i] = end;
        return true;
    }
    if (end == e) {
        end_[i] = start;
        return true;
    }
    // Split in two: needs one more slot.
    if (count_ >= kMaxRanges)
        return false;
    for (u32 k = count_; k > i + 1; --k) {
        base_[k] = base_[k - 1];
        end_[k] = end_[k - 1];
    }
    end_[i] = start;
    base_[i + 1] = end;
    end_[i + 1] = e;
    ++count_;
    return true;
}

u32 MemoryMap::claim(u32 virt, u32 size, u32 align)
{
    if (size == 0)
        return kFail;
    if (align == 0) {
        const u64 start = virt, end = start + size;
        for (u32 i = 0; i < count_; ++i)
            if (base_[i] <= start && end <= end_[i])
                return take(i, start, end) ? virt : kFail;
        return kFail;
    }
    if (align & (align - 1))
        return kFail;  // not a power of two
    for (u32 i = 0; i < count_; ++i) {
        const u64 start = (base_[i] + align - 1) & ~static_cast<u64>(align - 1);
        const u64 end = start + size;
        if (end <= end_[i] && end <= 0x100000000ull)
            return take(i, start, end) ? static_cast<u32>(start) : kFail;
    }
    return kFail;
}

bool MemoryMap::release(u32 virt, u32 size)
{
    return size != 0 && insert(virt, static_cast<u64>(virt) + size);
}

bool MemoryMap::is_free(u32 base, u32 size) const
{
    const u64 s = base, e = s + size;
    for (u32 i = 0; i < count_; ++i)
        if (base_[i] <= s && e <= end_[i])
            return true;
    return false;
}

u32 MemoryMap::free_bytes() const
{
    u64 n = 0;
    for (u32 i = 0; i < count_; ++i)
        n += end_[i] - base_[i];
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(n);
}

} // namespace g3::of
