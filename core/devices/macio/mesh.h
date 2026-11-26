// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/mesh.h — the MESH SCSI controller cell of mac-io
// (Paddington), with no devices on its SCSI bus yet.
//
// Facts (docs/hardware/mesh.md):
//  - Linux drivers/scsi/mesh.h (read for facts only): sixteen 8-bit
//    registers 0x10 apart — count lo/hi, FIFO, sequence, bus status 0/1,
//    FIFO count, exception, error, interrupt mask, interrupt, source ID,
//    destination ID, sync parameters, MESH ID, selection timeout. Sequence
//    commands (low 4 bits): 1 arbitrate, 2 select, 3 command, 4 status,
//    5 data out, 6 data in, 7 message out, 8 message in, 9 bus free,
//    A/B enable/disable parity, C/D enable/disable reselection, E reset
//    MESH, F flush FIFO; flags 0x80 DMA, 0x40 target, 0x20 ATN.
//    Exception bits: 0x01 selection timeout, 0x02 phase mismatch, 0x04
//    arbitration lost, ...; error bits: 0x10 sequence error, 0x20 SCSI
//    reset, ...; interrupt / mask bits: 0x01 command done, 0x02 exception,
//    0x04 error. Linux clears exception, error and interrupt bits by writing
//    1s, and asserts SCSI reset by writing 0x80 to bus status 1.
//  - A selection with no target answering ends in exception "selection
//    timeout" (Linux: DID_BAD_TARGET).
//  - MESH ID 4 for the MESH cell inside Heathrow (DingusPPC, read for facts;
//    Paddington derives from Heathrow — to verify).
// Our choices: commands complete at once (a selection times out without
// waiting sel_timeout x 10 ms); phase commands with no connection set a
// sequence error. The interrupt line is (interrupt & mask) != 0.

#ifndef G3_CORE_DEVICES_MACIO_MESH_H
#define G3_CORE_DEVICES_MACIO_MESH_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class MacioMesh final : public MmioDevice {
public:
    static constexpr u32 kWindowSize = 0x100;
    static constexpr u8 kMeshId = 4;
    enum Reg : u32 {
        COUNT_LO, COUNT_HI, FIFO, SEQUENCE, BUS_STATUS0, BUS_STATUS1, FIFO_COUNT, EXCEPTION, ERROR,
        INTR_MASK, INTERRUPT, SOURCE_ID, DEST_ID, SYNC_PARAMS, MESH_ID, SEL_TIMEOUT,
    };
    enum : u8 {
        INT_CMDDONE = 0x01, INT_EXCEPTION = 0x02, INT_ERROR = 0x04,
        EXC_SELTO = 0x01, ERR_SEQERR = 0x10, BS1_RST = 0x80,
    };

    using IrqCallback = void (*)(void* context, bool asserted);
    void set_irq(IrqCallback cb, void* ctx) { irq_ = cb; irq_ctx_ = ctx; }

    u8 read_reg(u32 reg);
    void write_reg(u32 reg, u8 value);
    bool irq_asserted() const { return (interrupt_ & intr_mask_ & 7) != 0; }

    // MmioDevice: register n at offset n * 0x10 (byte accesses).
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    void command(u8 seq);
    void update_irq();

    u16 count_ = 0;
    u8 fifo_[16] = {};
    u32 fifo_n_ = 0;
    u8 sequence_ = 0, bus1_ = 0, exception_ = 0, error_ = 0, intr_mask_ = 0, interrupt_ = 0;
    u8 source_id_ = 0, dest_id_ = 0, sync_ = 0, sel_timeout_ = 0;
    IrqCallback irq_ = nullptr;
    void* irq_ctx_ = nullptr;
    bool line_ = false;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_MESH_H
