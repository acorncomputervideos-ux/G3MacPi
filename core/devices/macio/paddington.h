// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/paddington.h — the Blue & White G3's mac-io chip
// ("Paddington", a Heathrow derivative) as a PCI function.
//
// Facts (docs/hardware/paddington.md):
//  - PCI IDs 106B:0017 "Paddington Mac I/O" (PCI ID database).
//  - One 512 KB memory BAR containing all mac-io cells; class 0xFF0000
//    (QEMU's Heathrow/mac-io model).
//  - Cell offsets inside the BAR (QEMU's Old World mac-io layout, which
//    matches the Heathrow family; NOT yet verified for Paddington):
//      0x00000 interrupt controller
//      0x16000 VIA (Cuda)
//    Other cells (DBDMA 0x08000, SCC 0x13000, IDE 0x20000/0x21000, ...)
//    are not modelled yet and read as unmapped.
// The cells are mapped on the physical bus whenever BAR 0 is non-zero and
// memory decoding is enabled in the command register, and moved/removed
// when software changes either.

#ifndef G3_CORE_DEVICES_MACIO_PADDINGTON_H
#define G3_CORE_DEVICES_MACIO_PADDINGTON_H

#include "core/devices/macio/cuda.h"
#include "core/devices/macio/heathrow_pic.h"
#include "core/devices/macio/via6522.h"
#include "core/devices/pci/pci.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"

namespace g3 {

class Paddington final : public PciFunction {
public:
    static constexpr u32 kBarSize = 0x80000;
    static constexpr u32 kPicOffset = 0x00000;
    static constexpr u32 kViaOffset = 0x16000;
    // Interrupt controller input used by the VIA/Cuda (QEMU Old World
    // OLDWORLD_CUDA_IRQ = 0x12; to verify for the B&W).
    static constexpr u32 kViaIrq = 18;

    Paddington(PhysicalBus& bus, Scheduler& scheduler, u8 revision = 0);

    HeathrowPic& pic() { return pic_; }
    Via6522& via() { return via_; }
    Cuda& cuda() { return cuda_; }
    u32 mapped_base() const { return mapped_base_; }

    // PciFunction
    u32 config_read(u32 reg) override { return cfg_.read(reg); }
    void config_write(u32 reg, u32 value, u32 byte_enables) override;

private:
    void update_mapping();
    void unmap_cells();

    PhysicalBus& bus_;
    PciConfigSpace cfg_;
    HeathrowPic pic_;
    Via6522 via_;
    Cuda cuda_;
    u32 mapped_base_ = 0;  // 0 = not mapped
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_PADDINGTON_H
