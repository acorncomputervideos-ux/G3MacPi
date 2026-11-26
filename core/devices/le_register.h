// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/le_register.h — accessing little-endian device registers
// from the big-endian PowerPC.
//
// Many Macintosh I/O chips (mac-io interrupt controller, PCI configuration
// data, ...) have little-endian 32-bit registers: register byte i (bits
// 8i..8i+7) lives at address offset i. The CPU reads n bytes starting at
// offset o and assembles them big-endian. These helpers implement exactly
// that byte-lane mapping for any size and offset inside the register, so
// software using lwbrx (byte-reversed) or plain lbz/lhz/lwz sees what it
// would on hardware.

#ifndef G3_CORE_DEVICES_LE_REGISTER_H
#define G3_CORE_DEVICES_LE_REGISTER_H

#include "core/common/types.h"

namespace g3 {

// Value seen by a big-endian CPU access of 'size' bytes at byte 'offset'
// (0..3) of little-endian register 'reg'. Bytes beyond the register read 0.
constexpr u32 le_reg_read(u32 reg, u32 offset, u32 size)
{
    u32 v = 0;
    for (u32 i = 0; i < size; ++i) {
        const u32 byte_index = offset + i;
        const u32 byte = byte_index < 4 ? (reg >> (8 * byte_index)) & 0xFF : 0;
        v = (v << 8) | byte;
    }
    return v;
}

// Register value after a big-endian CPU write of 'size' bytes at 'offset'.
constexpr u32 le_reg_write(u32 reg, u32 offset, u32 size, u32 value)
{
    for (u32 i = 0; i < size; ++i) {
        const u32 byte_index = offset + i;
        if (byte_index >= 4)
            break;
        const u32 byte = (value >> (8 * (size - 1 - i))) & 0xFF;
        reg = (reg & ~(0xFFu << (8 * byte_index))) | (byte << (8 * byte_index));
    }
    return reg;
}

} // namespace g3

#endif // G3_CORE_DEVICES_LE_REGISTER_H
