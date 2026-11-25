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
//      0x10000 MESH, 0x11000 BMAC+ Ethernet, 0x12000/0x13000 ESCC,
//      0x14000 sound (DAV bus, Burgundy codec)
//    Others (SWIM3 floppy 0x15000) are not modelled and read as unmapped.
// The cells are mapped on the physical bus whenever BAR 0 is non-zero and
// memory decoding is enabled in the command register, and moved/removed
// when software changes either.

#ifndef G3_CORE_DEVICES_MACIO_PADDINGTON_H
#define G3_CORE_DEVICES_MACIO_PADDINGTON_H

#include "core/devices/macio/bmac.h"
#include "core/devices/macio/cuda.h"
#include "core/devices/macio/dbdma.h"
#include "core/devices/macio/heathrow_pic.h"
#include "core/devices/macio/macio_ide.h"
#include "core/devices/macio/escc.h"
#include "core/devices/macio/mesh.h"
#include "core/devices/macio/nvram.h"
#include "core/devices/macio/davbus.h"
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
    // MESH SCSI cell and its interrupt (a real B&W: Linux shows MESH on
    // interrupt 12; registers at mac-io + 0x10000 as in Heathrow's layout).
    static constexpr u32 kMeshOffset = 0x10000;
    static constexpr u32 kMeshIrq = 12;
    // ESCC (Z85C30): legacy view at + 0x12000, MacRISC view at + 0x13000;
    // channel A on input 15, B on 16 (a real B&W's Linux log).
    static constexpr u32 kEsccOffset = 0x12000;
    static constexpr u32 kEsccIrq[2] = {15, 16};
    // The SCC's DBDMA: transmit/receive for A at + 0x8400/0x8500, for B at
    // + 0x8600/0x8700 (OpenBIOS drivers/escc.c), i.e. our controller's
    // channels 8/10 and 12/14 (0x80 apart); Heathrow DMA interrupts 4/5, 6/7.
    static constexpr u32 kEsccDmaChannel[2][2] = {{8, 10}, {12, 14}};
    static constexpr u32 kEsccDmaIrq[2][2] = {{4, 5}, {6, 7}};
    // Interrupt controller input used by the VIA/Cuda (QEMU Old World
    // OLDWORLD_CUDA_IRQ = 0x12; to verify for the B&W).
    static constexpr u32 kViaIrq = 18;
    // IDE cells (QEMU Old World layout and IRQs; to verify for the B&W).
    static constexpr u32 kIdeOffset[2] = {0x20000, 0x21000};
    static constexpr u32 kIdeDmaChannel[2] = {0x16, 0x1A};
    static constexpr u32 kIdeIrq[2] = {13, 14};
    static constexpr u32 kIdeDmaIrq[2] = {2, 3};

    // Sound (the DAV bus; its codec answers as a Screamer, which is what
    // Mac OS 9.2.1 drives here, see davbus.h) and Ethernet (BMAC+): offsets in the
    // BAR (DingusPPC MacIoTwo), interrupts as a real B&W's Linux log and
    // DingusPPC give them; DBDMA channels at mac-io + 0x8800/0x8900 and
    // + 0x8200/0x8300 (channel index = offset / 0x80 in our controller).
    // docs/hardware/davbus.md, docs/hardware/bmac.md.
    static constexpr u32 kBmacOffset = 0x11000;
    static constexpr u32 kBmacIrq = 42;
    static constexpr u32 kBmacDmaChannel[2] = {0x200 / 0x80, 0x300 / 0x80};  // tx, rx
    static constexpr u32 kBmacDmaIrq[2] = {32, 33};
    static constexpr u32 kSoundOffset = 0x14000;
    static constexpr u32 kSoundIrq = 17;
    static constexpr u32 kSoundDmaChannel[2] = {0x800 / 0x80, 0x900 / 0x80};  // out, in
    static constexpr u32 kSoundDmaIrq[2] = {8, 9};

    Paddington(PhysicalBus& bus, Scheduler& scheduler, u8 revision = 0);

    HeathrowPic& pic() { return pic_; }
    Via6522& via() { return via_; }
    Cuda& cuda() { return cuda_; }
    DbdmaController& dbdma() { return dbdma_; }
    MacioNvram& nvram() { return nvram_; }
    MacioIde& ide(u32 n) { return ide_[n & 1]; }
    MacioMesh& mesh() { return mesh_; }
    MacioEscc& escc() { return escc_; }
    MacioDavbus& sound() { return sound_; }
    MacioBmac& ethernet() { return bmac_; }
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
    MacioMesh mesh_;
    MacioEscc escc_;
    MacioDavbus sound_;
    MacioBmac bmac_;
    struct IrqRoute { HeathrowPic* pic; u32 irq; };
    IrqRoute ide_irq_[2] = {};
    IrqRoute ide_dma_irq_[2] = {};
    IrqRoute sound_dma_irq_[2] = {}, bmac_dma_irq_[2] = {};
    u32 mapped_base_ = 0;  // 0 = not mapped
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_PADDINGTON_H
