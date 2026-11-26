// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/pci/pci_bridge.h — transparent PCI-to-PCI bridge (type 1
// header), e.g. the DEC 21154 of the Power Macintosh G3 (Blue & White).
//
// Implemented from the PCI-to-PCI Bridge Architecture Specification:
//   0x18 primary bus number, 0x19 secondary, 0x1A subordinate,
//   0x1B secondary latency timer, 0x1C/0x1D I/O base/limit,
//   0x20/0x22 memory base/limit, 0x24/0x26 prefetchable base/limit,
//   0x3E bridge control. Header type 0x01, class 0x060400.
// Configuration cycles for bus numbers in [secondary, subordinate] are
// forwarded (PciBus::config_read_routed).
//
// Not modelled (yet): forwarding of memory/I/O transactions by window — the
// machine maps devices directly on the physical bus; secondary status
// write-1-to-clear bits; secondary bus reset; the 21154's chip-specific
// registers (0x40-0xFF are plain storage).

#ifndef G3_CORE_DEVICES_PCI_PCI_BRIDGE_H
#define G3_CORE_DEVICES_PCI_PCI_BRIDGE_H

#include "core/devices/pci/pci.h"

namespace g3 {

class PciToPciBridge final : public PciFunction {
public:
    PciToPciBridge(u16 vendor, u16 device, u8 revision);

    // DEC 21154 (PCI IDs 1011:0026). Revision of the B&W part: unknown.
    static PciToPciBridge dec21154(u8 revision = 0) { return PciToPciBridge(0x1011, 0x0026, revision); }

    PciToPciBridge* as_bridge() override { return this; }
    u32 config_read(u32 reg) override { return cfg_.read(reg); }
    void config_write(u32 reg, u32 value, u32 byte_enables) override
    {
        cfg_.write(reg, value, byte_enables);
    }

    PciBus& secondary_bus() { return secondary_; }
    u32 primary_number() const { return cfg_.get8(0x18); }
    u32 secondary_number() const { return cfg_.get8(0x19); }
    u32 subordinate_number() const { return cfg_.get8(0x1A); }

    // True if configuration cycles for 'bus' go through this bridge.
    bool forwards_bus(u32 bus) const
    {
        const u32 sec = secondary_number(), sub = subordinate_number();
        return sec != 0 && bus >= sec && bus <= sub;
    }

private:
    PciConfigSpace cfg_;
    PciBus secondary_;
};

} // namespace g3

#endif // G3_CORE_DEVICES_PCI_PCI_BRIDGE_H
