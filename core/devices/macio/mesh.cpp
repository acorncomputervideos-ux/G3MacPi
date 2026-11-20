// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/mesh.cpp — see mesh.h.

#include "core/devices/macio/mesh.h"

namespace g3 {

u8 MacioMesh::read_reg(u32 reg)
{
    switch (reg & 15) {
    case COUNT_LO: return static_cast<u8>(count_);
    case COUNT_HI: return static_cast<u8>(count_ >> 8);
    case FIFO: {
        if (fifo_n_ == 0)
            return 0;
        const u8 v = fifo_[0];
        for (u32 i = 1; i < fifo_n_; ++i)
            fifo_[i - 1] = fifo_[i];
        --fifo_n_;
        return v;
    }
    case SEQUENCE: return sequence_;
    case BUS_STATUS0: return 0;        // no target: no REQ/ACK/phase signals
    case BUS_STATUS1: return bus1_;    // RST while the host asserts it
    case FIFO_COUNT: return static_cast<u8>(fifo_n_);
    case EXCEPTION: return exception_;
    case ERROR: return error_;
    case INTR_MASK: return intr_mask_;
    case INTERRUPT: return interrupt_;
    case SOURCE_ID: return source_id_;
    case DEST_ID: return dest_id_;
    case SYNC_PARAMS: return sync_;
    case MESH_ID: return kMeshId;
    default: return sel_timeout_;
    }
}

void MacioMesh::write_reg(u32 reg, u8 v)
{
    switch (reg & 15) {
    case COUNT_LO: count_ = static_cast<u16>((count_ & 0xFF00) | v); break;
    case COUNT_HI: count_ = static_cast<u16>((count_ & 0x00FF) | (v << 8)); break;
    case FIFO:
        if (fifo_n_ < sizeof fifo_)
            fifo_[fifo_n_++] = v;
        break;
    case SEQUENCE: command(v); break;
    case BUS_STATUS1: bus1_ = v & BS1_RST; break;
    case EXCEPTION: exception_ &= static_cast<u8>(~v); break;  // write 1 to clear
    case ERROR: error_ &= static_cast<u8>(~v); break;
    case INTR_MASK: intr_mask_ = v & 7; update_irq(); break;
    case INTERRUPT: interrupt_ &= static_cast<u8>(~v); update_irq(); break;
    case SOURCE_ID: source_id_ = v; break;
    case DEST_ID: dest_id_ = v; break;
    case SYNC_PARAMS: sync_ = v; break;
    case SEL_TIMEOUT: sel_timeout_ = v; break;
    default: break;                    // bus status 0, FIFO count, MESH ID: read-only
    }
}

void MacioMesh::command(u8 seq)
{
    sequence_ = seq;
    switch (seq & 0x0F) {
    case 0x0:                          // no operation
        return;
    case 0x2:                          // select: nobody answers
        exception_ |= EXC_SELTO;
        interrupt_ |= INT_EXCEPTION | INT_CMDDONE;
        break;
    case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: case 0x8:
        error_ |= ERR_SEQERR;          // a bus phase without a connection
        interrupt_ |= INT_ERROR;
        break;
    case 0xE:                          // reset MESH
        count_ = 0;
        fifo_n_ = 0;
        exception_ = 0;
        error_ = 0;
        interrupt_ = INT_CMDDONE;
        break;
    case 0xF:                          // flush FIFO
        fifo_n_ = 0;
        interrupt_ |= INT_CMDDONE;
        break;
    default:                           // arbitrate (we win), bus free, parity, reselection
        interrupt_ |= INT_CMDDONE;
        break;
    }
    update_irq();
}

void MacioMesh::update_irq()
{
    const bool now = irq_asserted();
    if (now != line_) {
        line_ = now;
        if (irq_)
            irq_(irq_ctx_, now);
    }
}

u32 MacioMesh::mmio_read(u32 offset, u32 size)
{
    // Registers are bytes; a wider read returns the register in its low byte
    // lane of a big-endian value starting at `offset` (only the addressed byte matters).
    u32 v = 0;
    for (u32 i = 0; i < size; ++i)
        v = (v << 8) | ((((offset + i) & 0xF) == 0) ? read_reg((offset + i) >> 4) : 0);
    return v;
}

void MacioMesh::mmio_write(u32 offset, u32 size, u32 value)
{
    for (u32 i = 0; i < size; ++i)
        if (((offset + i) & 0xF) == 0)
            write_reg((offset + i) >> 4, static_cast<u8>(value >> (8 * (size - 1 - i))));
}

} // namespace g3
