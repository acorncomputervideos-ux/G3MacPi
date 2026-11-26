// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/ata/cmd646.h — CMD646U2 PCI Ultra ATA controller: the Blue &
// White G3's hard-disk controller (PCI 01:01.0 behind the DEC bridge; Linux
// boot log: "Silicon Image, Inc. PCI0646 (rev 07) (prog-if 8f)", irq 26).
//
// Facts (docs/hardware/cmd646.md):
//  - PCI IDs 1095:0646, revision 7, class 01 01 8F (IDE, bus master, both
//    channels native and programmable); interrupt pin A, wired to Paddington
//    input 26 (Linux log; DingusPPC's Yosemite model).
//  - BARs (PCI IDE Controller Specification 1.0): 0 primary command block
//    (8 bytes of I/O), 1 primary control block (4; alternate status /
//    device control at +2), 2/3 the same for the secondary channel, 4 bus
//    master (16).
//  - Bus master registers (SFF-8038i, PCI IDE bus mastering): per channel
//    (secondary at +8) command +0 (bit 0 start, bit 3 read = device to
//    memory), status +2 (bit 0 active, bit 1 error, bit 2 interrupt; bits 1
//    and 2 write-1-to-clear; bits 5/6 drive DMA capable), PRD table pointer
//    +4 (dword aligned). PRD entries: physical address, byte count (0 =
//    64 KB) and bit 15 of the last halfword = end of table.
//  - CMD646U2: MRDMODE at bus master +1 (also config 0x71): bit 2/3 channel
//    0/1 interrupt (write 1 to clear), bit 4/5 block that channel's
//    interrupt; UDIDETCR0 at +3 (Ultra DMA timing, stored).
//  - Device-specific configuration registers 0x50-0x7F (timing etc.) are
//    stored; they do not change the emulation.
//
// PCI I/O space is at 0xFE000000 on the B&W (Grackle); the controller maps
// its windows there while I/O decoding is enabled (command register bit 0).

#ifndef G3_CORE_DEVICES_ATA_CMD646_H
#define G3_CORE_DEVICES_ATA_CMD646_H

#include "core/common/types.h"
#include "core/devices/ata/ata_bus.h"
#include "core/devices/pci/pci.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class Cmd646 final : public PciFunction {
public:
    static constexpr u32 kPciIoBase = 0xFE000000u;  // PCI I/O space on the B&W
    static constexpr u16 kVendor = 0x1095, kDevice = 0x0646;
    static constexpr u8 kRevision = 0x07;
    static constexpr u32 kClass = 0x01018F;
    static constexpr u32 kBarSize[5] = {8, 4, 8, 4, 16};

    using IrqCallback = void (*)(void* context, bool asserted);

    explicit Cmd646(PhysicalBus& bus);

    AtaBus& channel(u32 n) { return chan_[n & 1].ata; }
    void set_irq(IrqCallback cb, void* context);
    bool irq_asserted() const { return irq_; }
    u32 io_base(u32 bar) const { return mapped_[bar]; }  // 0 = not mapped

    // PciFunction
    u32 config_read(u32 reg) override;
    void config_write(u32 reg, u32 value, u32 byte_enables) override;

private:
    struct Channel {
        Cmd646* owner = nullptr;
        u32 index = 0;
        AtaBus ata;
        u8 bm_command = 0, bm_status = 0;
        u32 prd = 0;
        bool intrq = false;
    };
    // One I/O window (BAR): forwards to the controller with its BAR number.
    struct Window final : MmioDevice {
        Cmd646* owner = nullptr;
        u32 bar = 0;
        u32 mmio_read(u32 offset, u32 size) override { return owner->io_read(bar, offset, size); }
        void mmio_write(u32 offset, u32 size, u32 value) override { owner->io_write(bar, offset, size, value); }
    };

    u32 io_read(u32 bar, u32 offset, u32 size);
    void io_write(u32 bar, u32 offset, u32 size, u32 value);
    u8 bm_read(u32 offset);
    void bm_write(u32 offset, u8 value);
    void update_mapping();
    void channel_irq(Channel& c, bool asserted);
    void update_irq();
    void run_dma(Channel& c);
    static void dma_request(void* ctx);

    PhysicalBus& bus_;
    PciConfigSpace cfg_;
    Channel chan_[2];
    Window window_[5];
    u32 mapped_[5] = {};
    u8 mrdmode_ = 0;
    u8 udidetcr0_ = 0;
    bool irq_ = false;
    IrqCallback irq_cb_ = nullptr;
    void* irq_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_DEVICES_ATA_CMD646_H
