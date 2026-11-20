// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/memory_map.h — memory the firmware hands out
// through the client interface's claim/release services.
//
// Facts (docs/firmware/open-firmware.md; IEEE 1275 + PowerPC binding):
//  - claim ( virt size align -- base ): align = 0 claims exactly
//    [virt, virt + size); otherwise the firmware picks a free area aligned
//    to `align` (a power of two). Failure returns -1.
//  - release ( virt size -- ) frees an area claimed earlier.
//
// Choices: sizes are not rounded; an aligned claim takes the lowest free
// area that fits, or the highest one with set_top_down(true) — used for
// physical memory so that low memory stays free for programs that must
// load at fixed addresses (Apple's Mac OS ROM file needs this:
// docs/firmware/virtual-mode.md).

#ifndef G3_CORE_FIRMWARE_OPENFIRM_MEMORY_MAP_H
#define G3_CORE_FIRMWARE_OPENFIRM_MEMORY_MAP_H

#include "core/common/types.h"

namespace g3::of {

class MemoryMap {
public:
    static constexpr u32 kMaxRanges = 64;
    static constexpr u32 kFail = 0xFFFFFFFFu;

    // Adds memory to the free pool (merging with neighbours).
    bool add_free(u32 base, u32 size);

    // Returns the base address, or kFail.
    u32 claim(u32 virt, u32 size, u32 align);
    // Returns false if [virt, virt + size) overlaps free memory.
    bool release(u32 virt, u32 size);

    void set_top_down(bool t) { top_down_ = t; }
    bool is_free(u32 base, u32 size) const;
    u32 range_count() const { return count_; }
    u32 free_bytes() const;
    // Free range i (ascending), e.g. for a /memory "available" property.
    u32 range_base(u32 i) const { return static_cast<u32>(base_[i]); }
    u32 range_size(u32 i) const { return static_cast<u32>(end_[i] - base_[i]); }

private:
    bool take(u32 index, u64 start, u64 end);  // removes [start, end) from range index
    bool insert(u64 base, u64 end);

    // Ascending, non-overlapping, non-adjacent free ranges [base, end).
    u64 base_[kMaxRanges] = {};
    u64 end_[kMaxRanges] = {};
    u32 count_ = 0;
    bool top_down_ = false;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_MEMORY_MAP_H
