// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/pci/pci.h — PCI configuration space and bus.
//
// Configuration space is little-endian (PCI Local Bus Specification). Every
// access arrives here as an aligned 32-bit dword plus a byte-enable mask
// (bit i = byte i of the dword), the way the bus carries it.

#ifndef G3_CORE_DEVICES_PCI_PCI_H
#define G3_CORE_DEVICES_PCI_PCI_H

#include "core/common/types.h"

namespace g3 {

class PciFunction {
public:
    // reg: dword-aligned offset 0x00..0xFC.
    virtual u32 config_read(u32 reg) = 0;
    virtual void config_write(u32 reg, u32 value, u32 byte_enables) = 0;

protected:
    ~PciFunction() = default;
};

// Standard 256-byte configuration space with per-byte write masks and BAR
// sizing. Devices embed one and add device-specific behaviour around it.
class PciConfigSpace {
public:
    // Type 0 header identity (read-only fields).
    void init_header(u16 vendor, u16 device, u8 revision, u32 class_code, u8 header_type = 0);

    // Defines BAR 'index' (0..5) of 'size' bytes (power of two >= 16 for
    // memory, >= 4 for I/O). Software discovers the size by writing all
    // ones and reading back (PCI spec 6.2.5.1).
    void define_bar(u32 index, u32 size, bool io);

    // Raw initialisation (bypasses write masks) and mask control.
    void set8(u32 off, u8 v) { bytes_[off & 0xFF] = v; }
    void set16(u32 off, u16 v) { set8(off, static_cast<u8>(v)); set8(off + 1, static_cast<u8>(v >> 8)); }
    void set32(u32 off, u32 v) { set16(off, static_cast<u16>(v)); set16(off + 2, static_cast<u16>(v >> 16)); }
    void set_writable(u32 off, u32 len, u8 mask = 0xFF);
    u8 get8(u32 off) const { return bytes_[off & 0xFF]; }
    u16 get16(u32 off) const { return static_cast<u16>(get8(off) | (get8(off + 1) << 8)); }
    u32 get32(u32 off) const { return get16(off) | (static_cast<u32>(get16(off + 2)) << 16); }

    u32 read(u32 reg) const { return get32(reg & 0xFC); }
    void write(u32 reg, u32 value, u32 byte_enables);

private:
    u8 bytes_[256] = {};
    u8 writable_[256] = {};
};

class PciBus {
public:
    static constexpr u32 kDevices = 32;
    static constexpr u32 kFunctions = 8;

    // devfn = device << 3 | function. Returns false if occupied.
    bool attach(u32 devfn, PciFunction* f);
    PciFunction* function(u32 devfn) const { return devfn < 256 ? slots_[devfn] : nullptr; }

    // Absent functions read all ones (master abort) and ignore writes.
    u32 config_read(u32 devfn, u32 reg) const;
    void config_write(u32 devfn, u32 reg, u32 value, u32 byte_enables);

private:
    PciFunction* slots_[256] = {};
};

} // namespace g3

#endif // G3_CORE_DEVICES_PCI_PCI_H
