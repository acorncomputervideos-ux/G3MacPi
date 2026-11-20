// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/pci/pci_bridge.cpp — see pci_bridge.h.

#include "core/devices/pci/pci_bridge.h"

namespace g3 {

PciToPciBridge::PciToPciBridge(u16 vendor, u16 device, u8 revision)
{
    cfg_.init_header(vendor, device, revision, 0x060400, 0x01);
    cfg_.set_writable(0x18, 4);          // primary/secondary/subordinate bus, sec. latency
    // I/O base/limit: bits 7-4 writable, bits 3-0 = 0 (16-bit I/O addressing).
    cfg_.set_writable(0x1C, 2, 0xF0);
    // Memory and prefetchable base/limit: bits 15-4 writable, 3-0 read-only
    // (0 = 32-bit addressing for the prefetchable range).
    static const u32 kWindows[4] = {0x20, 0x22, 0x24, 0x26};
    for (u32 off : kWindows) {
        cfg_.set_writable(off, 1, 0xF0);
        cfg_.set_writable(off + 1, 1, 0xFF);
    }
    // Bridge control (0x3E-0x3F): parity response, SERR#, ISA, VGA, master
    // abort mode, secondary bus reset, fast back-to-back; upper byte: the
    // discard-timer bits of the 21154 family.
    cfg_.set_writable(0x3E, 1, 0xEF);
    cfg_.set_writable(0x3F, 1, 0x0F);
    // Chip-specific registers: plain storage until modelled.
    cfg_.set_writable(0x40, 0xC0);
}

} // namespace g3
