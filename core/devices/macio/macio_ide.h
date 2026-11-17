// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/macio_ide.h — the mac-io ATA cell ("heathrow-ata"
// family) in front of an AtaBus, with a DBDMA channel for DMA transfers.
//
// Facts (docs/hardware/ata.md; Linux pata_macio.c, QEMU hw/ide/macio.c):
//  - Little-endian register window; taskfile register n at offset n * 0x10
//    (n = 0..7), alternate status / device control at 0x160.
//  - Data register: 16- or 32-bit accesses; the byte stream keeps its order
//    in memory (Linux uses ioread16_rep, i.e. no byte swap).
//  - Taskfile registers 1-7 and 0x160 are byte registers.
//  - 32-bit timing register at 0x200 (stored; it has no effect here).
//  - Linux uses no interrupt register at 0x300 on Heathrow-class cells
//    (it is KeyLargo and later only).
//  - DMA: Linux builds INPUT/OUTPUT_MORE..LAST + STOP descriptors, sets
//    cmdptr, issues the ATA command, then sets RUN. Done when the status
//    shows RUN, no DEAD and no ACTIVE.

#ifndef G3_CORE_DEVICES_MACIO_MACIO_IDE_H
#define G3_CORE_DEVICES_MACIO_MACIO_IDE_H

#include "core/devices/ata/ata_bus.h"
#include "core/devices/macio/dbdma.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class MacioIde final : public MmioDevice, public DbdmaStream {
public:
    static constexpr u32 kWindowSize = 0x1000;
    static constexpr u32 kAltStatusOffset = 0x160;
    static constexpr u32 kTimingOffset = 0x200;

    MacioIde();

    AtaBus& bus() { return ata_; }
    // Connects the DBDMA channel that serves this cell.
    void attach_dma(DbdmaChannel* channel);
    // The ATA interrupt (INTRQ) output.
    void set_irq(AtaBus::IrqCallback cb, void* context) { ata_.set_irq(cb, context); }
    u32 timing() const { return timing_; }

    // MmioDevice
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

    // DbdmaStream
    u32 dma_output(const u8* data, u32 len, bool last) override;
    u32 dma_input(u8* data, u32 len) override;

private:
    static void dma_request(void* ctx);

    AtaBus ata_;
    DbdmaChannel* dma_ = nullptr;
    u32 timing_ = 0;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_MACIO_IDE_H
