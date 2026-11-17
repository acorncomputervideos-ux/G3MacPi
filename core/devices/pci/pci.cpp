// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/pci/pci.cpp — see pci.h.

#include "core/devices/pci/pci.h"

namespace g3 {

void PciConfigSpace::init_header(u16 vendor, u16 device, u8 revision, u32 class_code,
                                 u8 header_type)
{
    set16(0x00, vendor);
    set16(0x02, device);
    set8(0x08, revision);
    set8(0x09, static_cast<u8>(class_code));        // programming interface
    set8(0x0A, static_cast<u8>(class_code >> 8));   // sub-class
    set8(0x0B, static_cast<u8>(class_code >> 16));  // base class
    set8(0x0E, header_type);
    // Command register: I/O space, memory space, bus master, MWI,
    // parity error response, SERR# enable (PCI spec 6.2.2).
    set_writable(0x04, 1, 0x57);
    set_writable(0x05, 1, 0x01);
    set_writable(0x0C, 1);  // cache line size
    set_writable(0x0D, 1);  // latency timer
    set_writable(0x3C, 1);  // interrupt line (scratch for software)
}

void PciConfigSpace::set_writable(u32 off, u32 len, u8 mask)
{
    for (u32 i = 0; i < len; ++i)
        writable_[(off + i) & 0xFF] = mask;
}

void PciConfigSpace::define_bar(u32 index, u32 size, bool io)
{
    const u32 off = 0x10 + 4 * index;
    // Address bits below 'size' are read-only zero; the type bits are fixed.
    const u32 type = io ? 0x1u : 0x0u;  // memory: 32-bit, non-prefetchable
    set32(off, type);
    const u32 mask = ~(size - 1) & (io ? ~0x3u : ~0xFu);
    for (u32 i = 0; i < 4; ++i)
        writable_[off + i] = static_cast<u8>(mask >> (8 * i));
}

void PciConfigSpace::write(u32 reg, u32 value, u32 byte_enables)
{
    const u32 base = reg & 0xFC;
    for (u32 i = 0; i < 4; ++i) {
        if (!(byte_enables & (1u << i)))
            continue;
        const u8 m = writable_[base + i];
        const u8 v = static_cast<u8>(value >> (8 * i));
        bytes_[base + i] = static_cast<u8>((bytes_[base + i] & ~m) | (v & m));
    }
}

bool PciBus::attach(u32 devfn, PciFunction* f)
{
    if (devfn >= 256 || slots_[devfn] || !f)
        return false;
    slots_[devfn] = f;
    return true;
}

u32 PciBus::config_read(u32 devfn, u32 reg) const
{
    PciFunction* f = function(devfn);
    return f ? f->config_read(reg & 0xFC) : 0xFFFFFFFFu;
}

void PciBus::config_write(u32 devfn, u32 reg, u32 value, u32 byte_enables)
{
    if (PciFunction* f = function(devfn))
        f->config_write(reg & 0xFC, value, byte_enables);
}

} // namespace g3
