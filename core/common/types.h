// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/common/types.h — fixed-width integer types for the portable emulator core.
//
// Core code must build both on Linux (tests) and inside Circle, where the C++
// standard library headers are NOT available (-nostdinc++, -ffreestanding).
// Therefore core code may only use the freestanding C headers <stdint.h> and
// <stddef.h> plus compiler builtins.

#ifndef G3_CORE_COMMON_TYPES_H
#define G3_CORE_COMMON_TYPES_H

#include <stddef.h>
#include <stdint.h>

namespace g3 {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

} // namespace g3

#endif // G3_CORE_COMMON_TYPES_H
