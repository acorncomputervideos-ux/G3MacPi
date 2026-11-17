// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/pci/grackle.h — Motorola MPC106 ("Grackle") PCI host bridge
// as used in the Power Macintosh G3 (Blue & White): configuration-space
// access mechanism and the bridge's own configuration header.
//
// Facts used (docs/hardware/grackle.md):
//  - CONFIG_ADDRESS port at 0xFEC00000, CONFIG_DATA port at 0xFEE00000
//    (Linux setup_grackle()).
//  - Both are little-endian registers; CONFIG_ADDRESS has the standard PCI
//    layout: bit 31 enable, bits 23-16 bus, 15-8 device/function, 7-2
//    register (consistent with Linux's GRACKLE_CFA() written big-endian).
//  - A byte/halfword access at CONFIG_DATA + n addresses configuration byte
//    (register + n).
//  - The bridge itself is bus 0, device 0, function 0; PCI IDs 1057:0002
//    (Motorola MPC106); class 0x060000 (host bridge).
// Not modelled yet: the memory-controller registers (0x80-0xFF are plain
// storage), PCI-to-memory/ROM address maps, error reporting, bus > 0
// (needs the DEC21154 bridge model).

#ifndef G3_CORE_DEVICES_PCI_GRACKLE_H
#define G3_CORE_DEVICES_PCI_GRACKLE_H

#include "core/devices/pci/pci.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class Grackle final : public PciFunction {
public:
    static constexpr u32 kConfigAddressBase = 0xFEC00000u;
    static constexpr u32 kConfigDataBase = 0xFEE00000u;
    static constexpr u32 kPortWindow = 0x1000;  // full decode range: to verify

    explicit Grackle(u8 revision = 0);  // revision of the B&W part: unknown

    // Maps both configuration ports and attaches the bridge as 00:00.0.
    bool attach(PhysicalBus& bus);

    PciBus& pci_bus() { return bus0_; }
    u32 config_address() const { return config_address_; }

    // PciFunction (the bridge's own configuration header)
    u32 config_read(u32 reg) override { return cfg_.read(reg); }
    void config_write(u32 reg, u32 value, u32 byte_enables) override
    {
        cfg_.write(reg, value, byte_enables);
    }

private:
    struct AddressPort final : MmioDevice {
        Grackle* g;
        u32 mmio_read(u32 offset, u32 size) override;
        void mmio_write(u32 offset, u32 size, u32 value) override;
    };
    struct DataPort final : MmioDevice {
        Grackle* g;
        u32 mmio_read(u32 offset, u32 size) override;
        void mmio_write(u32 offset, u32 size, u32 value) override;
    };

    u32 data_read(u32 offset, u32 size);
    void data_write(u32 offset, u32 size, u32 value);

    PciConfigSpace cfg_;
    PciBus bus0_;
    u32 config_address_ = 0;
    AddressPort address_port_;
    DataPort data_port_;
};

} // namespace g3

#endif // G3_CORE_DEVICES_PCI_GRACKLE_H
