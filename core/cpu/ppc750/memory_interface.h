// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/cpu/ppc750/memory_interface.h — how the CPU reaches memory.
//
// Addresses are physical. (Milestone 3 adds BAT/page-table translation in
// front of this interface.) Values are in host order; implementations deal
// with the guest's big-endian byte order. An access that hits nothing
// returns false: the CPU turns that into a machine check, which is what a
// bus error (TEA) causes on a real 750.

#ifndef G3_CORE_CPU_PPC750_MEMORY_INTERFACE_H
#define G3_CORE_CPU_PPC750_MEMORY_INTERFACE_H

#include "core/common/types.h"

namespace g3::ppc {

class MemoryInterface {
public:
    virtual bool read8(u32 addr, u8& value) = 0;
    virtual bool read16(u32 addr, u16& value) = 0;   // big-endian, any alignment
    virtual bool read32(u32 addr, u32& value) = 0;   // big-endian, any alignment
    virtual bool write8(u32 addr, u8 value) = 0;
    virtual bool write16(u32 addr, u16 value) = 0;
    virtual bool write32(u32 addr, u32 value) = 0;
    virtual bool fetch32(u32 addr, u32& insn) { return read32(addr, insn); }

protected:
    ~MemoryInterface() = default;
};

} // namespace g3::ppc

#endif // G3_CORE_CPU_PPC750_MEMORY_INTERFACE_H
