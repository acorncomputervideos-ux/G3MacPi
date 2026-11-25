// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/dbdma.cpp — see dbdma.h.

#include "core/devices/macio/dbdma.h"

#include "core/devices/le_register.h"

namespace g3 {

namespace {
// Register offsets inside a channel.
constexpr u32 kControl = 0x00, kStatus = 0x04, kCmdPtrHi = 0x08, kCmdPtr = 0x0C;
constexpr u32 kIntrSel = 0x10, kBrSel = 0x14, kWaitSel = 0x18;
// Safety valve against descriptor loops (e.g. branch-to-self with NOP):
// processing pauses after this many descriptors per call.
constexpr u32 kMaxDescriptorsPerRun = 4096;
} // namespace

// --- memory helpers (descriptors are little-endian) ---------------------------------

u16 DbdmaChannel::rd16(u32 a)
{
    u8 b0 = 0, b1 = 0;
    bus_->read8(a, b0);
    bus_->read8(a + 1, b1);
    return static_cast<u16>(b0 | (b1 << 8));
}

u32 DbdmaChannel::rd32(u32 a) { return rd16(a) | (static_cast<u32>(rd16(a + 2)) << 16); }

void DbdmaChannel::wr16(u32 a, u16 v)
{
    bus_->write8(a, static_cast<u8>(v));
    bus_->write8(a + 1, static_cast<u8>(v >> 8));
}

// --- registers -----------------------------------------------------------------------------

u32 DbdmaChannel::read_reg(u32 reg) const
{
    switch (reg) {
    case kControl:  // reads return the status (QEMU; Linux never reads it)
    case kStatus: return status_;
    case kCmdPtrHi: return 0;
    case kCmdPtr: return cmdptr_;
    case kIntrSel: return intr_sel_;
    case kBrSel: return br_sel_;
    case kWaitSel: return wait_sel_;
    default: return 0;
    }
}

void DbdmaChannel::write_reg(u32 reg, u32 value)
{
    switch (reg) {
    case kControl: {
        const u32 mask = value >> 16;
        const u32 v = value & 0xFFFF;
        // Software-writable: RUN, PAUSE, FLUSH, WAKE and the device-status
        // bits. DEAD, ACTIVE and BT are under hardware control.
        const u32 writable = mask & (dbdma::RUN | dbdma::PAUSE | dbdma::FLUSH | dbdma::WAKE |
                                     dbdma::DEVSTAT);
        const bool was_running = status_ & dbdma::RUN;
        status_ = (status_ & ~writable) | (v & writable);
        if ((mask & dbdma::RUN) && !(v & dbdma::RUN)) {
            // Stopping the channel: also clears DEAD and ACTIVE.
            status_ &= ~(dbdma::DEAD | dbdma::ACTIVE | dbdma::WAKE);
            progress_ = 0;
            return;
        }
        // FLUSH: the current transfer descriptor gets its progress so far
        // (res_count, and xfer_status with ACTIVE and FLUSH set), then the
        // bit clears (nothing is buffered in this model). Mac OS 9's serial
        // driver (SerialDMA) relies on it: it sets FLUSH, waits for the bit
        // to clear, then reads the receive descriptor's status for ACTIVE
        // and its residual count for the bytes received so far.
        if ((mask & dbdma::FLUSH) && (v & dbdma::FLUSH) && (status_ & dbdma::ACTIVE))
            flush_progress();
        status_ &= ~dbdma::FLUSH;
        const bool start = (status_ & dbdma::RUN) && !was_running;
        const bool wake = (status_ & dbdma::WAKE) && (status_ & dbdma::RUN);
        status_ &= ~dbdma::WAKE;
        if ((start || wake) && !(status_ & dbdma::DEAD))
            status_ |= dbdma::ACTIVE;
        run();
        return;
    }
    case kStatus:
        return;  // read-only
    case kCmdPtr:
        if (!(status_ & dbdma::ACTIVE)) {
            cmdptr_ = value & ~0xFu;
            progress_ = 0;
        }
        return;
    case kIntrSel: intr_sel_ = value & 0x000F000Fu; return;
    case kBrSel: br_sel_ = value & 0x000F000Fu; return;
    case kWaitSel: wait_sel_ = value & 0x000F000Fu; return;
    default: return;
    }
}

void DbdmaChannel::set_device_status(u8 bits)
{
    status_ = (status_ & ~dbdma::DEVSTAT) | bits;
    run();  // a waiting descriptor may now proceed
}

void DbdmaChannel::kick() { run(); }

void DbdmaChannel::flush_progress()
{
    if (!bus_)
        return;
    const u32 desc = cmdptr_;
    const u16 cmd = rd16(desc + 2) >> 12;
    if (cmd > dbdma::INPUT_LAST)
        return;  // not a transfer: nothing in progress
    const u16 req = rd16(desc + 0);
    wr16(desc + 12, static_cast<u16>(req - (progress_ < req ? progress_ : req)));
    wr16(desc + 14, static_cast<u16>(status_ | dbdma::ACTIVE | dbdma::FLUSH));
}

bool DbdmaChannel::condition(u32 sel) const
{
    const u32 mask = (sel >> 16) & 0x0F;
    const u32 value = sel & 0x0F;
    return (status_ & mask) == (value & mask);
}

void DbdmaChannel::kill()
{
    status_ |= dbdma::DEAD;
    status_ &= ~dbdma::ACTIVE;
    if (irq_cb_)
        irq_cb_(irq_ctx_);
}

// --- execution ----------------------------------------------------------------------------

void DbdmaChannel::run()
{
    if (running_ || !bus_)
        return;
    running_ = true;
    for (u32 n = 0; n < kMaxDescriptorsPerRun; ++n) {
        if (!(status_ & dbdma::RUN) || !(status_ & dbdma::ACTIVE) || (status_ & dbdma::PAUSE) ||
            (status_ & dbdma::DEAD))
            break;
        if (!step())
            break;
    }
    running_ = false;
}

void DbdmaChannel::finish(u32 desc, u16 cmdword, u32 cmd_dep, u16 residual, bool allow_branch)
{
    // Write back the transfer status and residual count.
    wr16(desc + 12, residual);
    wr16(desc + 14, static_cast<u16>(status_ | dbdma::ACTIVE));
    progress_ = 0;

    const u32 intr = (cmdword >> 4) & 3;
    const bool raise = intr == 3 || (intr == 1 && condition(intr_sel_)) ||
                       (intr == 2 && !condition(intr_sel_));
    bool take_branch = false;
    if (allow_branch) {
        const u32 br = (cmdword >> 2) & 3;
        take_branch = br == 3 || (br == 1 && condition(br_sel_)) || (br == 2 && !condition(br_sel_));
    }
    if (take_branch) {
        cmdptr_ = cmd_dep & ~0xFu;
        status_ |= dbdma::BT;
    } else {
        cmdptr_ += 16;
        status_ &= ~dbdma::BT;
    }
    if (raise && irq_cb_)
        irq_cb_(irq_ctx_);
}

bool DbdmaChannel::step()
{
    const u32 desc = cmdptr_;
    const u16 req_count = rd16(desc + 0);
    const u16 cmdword = rd16(desc + 2);
    const u32 addr = rd32(desc + 4);
    const u32 cmd_dep = rd32(desc + 8);
    const u16 cmd = cmdword >> 12;
    const u16 key = (cmdword >> 8) & 7;
    const u32 wait = cmdword & 3;

    // Wait condition: stay on this descriptor until it no longer holds.
    if (wait == 3 || (wait == 1 && condition(wait_sel_)) || (wait == 2 && !condition(wait_sel_)))
        return false;

    switch (cmd) {
    case dbdma::OUTPUT_MORE:
    case dbdma::OUTPUT_LAST:
    case dbdma::INPUT_MORE:
    case dbdma::INPUT_LAST: {
        if (key > 3 || addr == 0) {  // only streams 0-3; null address
            kill();
            return false;
        }
        const bool output = cmd <= dbdma::OUTPUT_LAST;
        const bool last = cmd == dbdma::OUTPUT_LAST || cmd == dbdma::INPUT_LAST;
        if (!stream_) {
            kill();
            return false;
        }
        while (progress_ < req_count) {
            const u32 left = req_count - progress_;
            u8* p = bus_->host_pointer(addr + progress_, left);
            u8 tmp[256];
            u32 chunk = left;
            u32 done;
            if (p) {
                done = output ? stream_->dma_output(p, chunk, last) : stream_->dma_input(p, chunk);
                if (!output)
                    bus_->notify_write(addr + progress_, done);
            } else {
                // Not plain RAM: go through the bus in small pieces.
                chunk = left > sizeof tmp ? static_cast<u32>(sizeof tmp) : left;
                if (output) {
                    for (u32 i = 0; i < chunk; ++i)
                        bus_->read8(addr + progress_ + i, tmp[i]);
                    done = stream_->dma_output(tmp, chunk, last && chunk == left);
                } else {
                    done = stream_->dma_input(tmp, chunk);
                    for (u32 i = 0; i < done; ++i)
                        bus_->write8(addr + progress_ + i, tmp[i]);
                }
            }
            progress_ += done;
            if (done < chunk) {
                if (!output && last && stream_->dma_input_packet_end()) {
                    finish(desc, cmdword, cmd_dep, static_cast<u16>(req_count - progress_), true);
                    return true;
                }
                return false;  // device not ready: resume on kick()
            }
        }
        if (!output && last)
            stream_->dma_input_packet_end();  // full: a longer packet's rest is dropped
        finish(desc, cmdword, cmd_dep, 0, true);
        return true;
    }
    case dbdma::STORE_WORD:
    case dbdma::LOAD_WORD: {
        if (key != dbdma::KEY_SYSTEM) {
            kill();
            return false;
        }
        const u32 len = (req_count & 4) ? 4 : ((req_count & 2) ? 2 : 1);
        const u32 a = addr & ~(len - 1);
        if (cmd == dbdma::STORE_WORD) {
            // Write len bytes of cmd_dep (little-endian order) to memory.
            for (u32 i = 0; i < len; ++i)
                bus_->write8(a + i, static_cast<u8>(cmd_dep >> (8 * i)));
        } else {
            u32 v = 0;
            for (u32 i = 0; i < len; ++i) {
                u8 b = 0;
                bus_->read8(a + i, b);
                v |= static_cast<u32>(b) << (8 * i);
            }
            // The loaded value replaces cmd_dep in the descriptor.
            for (u32 i = 0; i < 4; ++i)
                bus_->write8(desc + 8 + i, static_cast<u8>(v >> (8 * i)));
        }
        finish(desc, cmdword, cmd_dep, 0, false);
        return true;
    }
    case dbdma::NOP:
        finish(desc, cmdword, cmd_dep, 0, true);
        return true;
    case dbdma::STOP:
        // Suspend without advancing: software may overwrite this
        // descriptor and set WAKE to continue from it.
        status_ &= ~dbdma::ACTIVE;
        return false;
    default:
        kill();
        return false;
    }
}

// --- controller ----------------------------------------------------------------------------

DbdmaController::DbdmaController(PhysicalBus& bus)
{
    for (u32 i = 0; i < kChannels; ++i)
        ch_[i].init(&bus, i);
}

u32 DbdmaController::mmio_read(u32 offset, u32 size)
{
    const u32 reg = (offset % kChannelStride) & ~3u;
    const u32 v = ch_[offset / kChannelStride].read_reg(reg);
    return le_reg_read(v, offset & 3, size);
}

void DbdmaController::mmio_write(u32 offset, u32 size, u32 value)
{
    if (size != 4 || (offset & 3))
        return;  // DBDMA registers are written as whole 32-bit words
    const u32 reg = (offset % kChannelStride) & ~3u;
    ch_[offset / kChannelStride].write_reg(reg, le_reg_write(0, 0, 4, value));
}

} // namespace g3
