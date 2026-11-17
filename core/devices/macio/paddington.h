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
//      0x08000 DBDMA controller (32 channels x 0x80)
//      0x16000 VIA (Cuda)
//      0x20000 IDE0, 0x21000 IDE1 (DBDMA channels 0x16 and 0x1A)
//      0x60000 NVRAM (8 KB, one byte every 16 bytes)
//    Other cells (SCC 0x13000, ...)
//    are not modelled yet and read as unmapped.
// The cells are mapped on the physical bus whenever BAR 0 is non-zero and
// memory decoding is enabled in the command register, and moved/removed
// when software changes either.

#ifndef G3_CORE_DEVICES_MACIO_PADDINGTON_H
#define G3_CORE_DEVICES_MACIO_PADDINGTON_H

#include "core/devices/macio/cuda.h"
#include "core/devices/macio/dbdma.h"
#include "core/devices/macio/heathrow_pic.h"
#include "core/devices/macio/macio_ide.h"
#include "core/devices/macio/nvram.h"
#include "core/devices/macio/via6522.h"
#include "core/devices/pci/pci.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"

namespace g3 {

class Paddington final : public PciFunction {
public:
    static constexpr u32 kBarSize = 0x80000;
    static constexpr u32 kPicOffset = 0x00000;
    static constexpr u32 kDbdmaOffset = 0x08000;
    static constexpr u32 kViaOffset = 0x16000;
    static constexpr u32 kNvramOffset = 0x60000;
    // Interrupt controller input used by the VIA/Cuda (QEMU Old World
    // OLDWORLD_CUDA_IRQ = 0x12; to verify for the B&W).
    static constexpr u32 kViaIrq = 18;
    // IDE cells (QEMU Old World layout and IRQs; to verify for the B&W).
    static constexpr u32 kIdeOffset[2] = {0x20000, 0x21000};
    static constexpr u32 kIdeDmaChannel[2] = {0x16, 0x1A};
    static constexpr u32 kIdeIrq[2] = {13, 14};
    static constexpr u32 kIdeDmaIrq[2] = {2, 3};

    Paddington(PhysicalBus& bus, Scheduler& scheduler, u8 revision = 0);

    HeathrowPic& pic() { return pic_; }
    Via6522& via() { return via_; }
    Cuda& cuda() { return cuda_; }
    DbdmaController& dbdma() { return dbdma_; }
    MacioNvram& nvram() { return nvram_; }
    MacioIde& ide(u32 n) { return ide_[n & 1]; }
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
    DbdmaController dbdma_;
    MacioNvram nvram_;
    MacioIde ide_[2];
    struct IrqRoute { HeathrowPic* pic; u32 irq; };
    IrqRoute ide_irq_[2] = {};
    IrqRoute ide_dma_irq_[2] = {};
    u32 mapped_base_ = 0;  // 0 = not mapped
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_PADDINGTON_H
