// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/dbdma.h — Apple Descriptor-Based DMA (DBDMA)
// controller of the mac-io chip.
//
// Facts (docs/hardware/dbdma.md; Linux asm/dbdma.h + QEMU mac_dbdma.c):
//  - Per channel, little-endian 32-bit registers: 0x00 control (write: mask
//    in bits 31-16, value in 15-0; read returns status), 0x04 status,
//    0x08 cmdptr_hi, 0x0C cmdptr, 0x10 intr_sel, 0x14 br_sel, 0x18 wait_sel.
//  - Status bits: RUN 0x8000, PAUSE 0x4000, FLUSH 0x2000, WAKE 0x1000,
//    DEAD 0x0800, ACTIVE 0x0400, BT 0x0100, device status 0x00FF.
//  - 16-byte little-endian descriptors: req_count (16), command (16),
//    phy_addr (32), cmd_dep (32), res_count (16), xfer_status (16).
//  - Commands: OUTPUT_MORE/LAST, INPUT_MORE/LAST, STORE_WORD, LOAD_WORD,
//    NOP, STOP; key, interrupt (bits 5-4), branch (3-2), wait (1-0) fields.
//  - Condition = (devstat & sel_mask) == (sel_value & sel_mask), 4-bit
//    selects (mask in bits 19-16, value in bits 3-0).
//  - Channels 0x80 apart; the controller window is 0x1000 (32 channels)
//    (QEMU DBDMA_CHANNEL_SHIFT 7; Linux struct dbdma_regs is 0x40 bytes,
//    which fits). E.g. IDE0 uses channel 0x16 = window offset 0xB00.
//
// Simplification: transfers complete in zero guest time when the device
// has data/space ready; a device that is not ready leaves the channel
// waiting on the descriptor until it calls DbdmaChannel::kick().

#ifndef G3_CORE_DEVICES_MACIO_DBDMA_H
#define G3_CORE_DEVICES_MACIO_DBDMA_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"

namespace g3 {

// The device at the other end of a channel's data stream.
class DbdmaStream {
public:
    // Memory -> device. Returns the number of bytes the device accepted
    // (0..len); fewer than len means "not ready, call kick() later".
    virtual u32 dma_output(const u8* data, u32 len, bool last) = 0;
    // Device -> memory. Returns the number of bytes delivered (0..len).
    virtual u32 dma_input(u8* data, u32 len) = 0;
    // Packet devices (Ethernet): an INPUT_LAST descriptor that ran out of
    // data, or is full, asks whether the device's packet has ended. true
    // completes the descriptor with the bytes not filled as its residual
    // count (a packet shorter than the buffer); the device then starts its
    // next packet in the next descriptor, and drops the rest of a packet
    // that did not fit. Stream devices keep the default: a short transfer
    // just waits for more data.
    virtual bool dma_input_packet_end() { return false; }

protected:
    ~DbdmaStream() = default;
};

namespace dbdma {
constexpr u32 RUN = 0x8000, PAUSE = 0x4000, FLUSH = 0x2000, WAKE = 0x1000;
constexpr u32 DEAD = 0x0800, ACTIVE = 0x0400, BT = 0x0100, DEVSTAT = 0x00FF;
constexpr u16 OUTPUT_MORE = 0, OUTPUT_LAST = 1, INPUT_MORE = 2, INPUT_LAST = 3;
constexpr u16 STORE_WORD = 4, LOAD_WORD = 5, NOP = 6, STOP = 7;
constexpr u16 KEY_STREAM0 = 0, KEY_REGS = 5, KEY_SYSTEM = 6, KEY_DEVICE = 7;
} // namespace dbdma

class DbdmaChannel {
public:
    using IrqCallback = void (*)(void* context);

    void init(PhysicalBus* bus, u32 index) { bus_ = bus; index_ = index; }
    void attach_stream(DbdmaStream* s) { stream_ = s; }
    void set_irq(IrqCallback cb, void* context) { irq_cb_ = cb; irq_ctx_ = context; }

    // Device side.
    void set_device_status(u8 bits);  // the 8 device-status bits
    void kick();                      // device became ready: continue

    u32 status() const { return status_; }
    u32 command_pointer() const { return cmdptr_; }

    u32 read_reg(u32 reg) const;
    void write_reg(u32 reg, u32 value);

private:
    void run();
    bool step();  // executes the current descriptor; false = stop processing
    bool condition(u32 sel) const;
    void finish(u32 desc, u16 cmdword, u32 cmd_dep, u16 residual, bool allow_branch);
    void kill();
    void flush_progress();
    u16 rd16(u32 a);
    u32 rd32(u32 a);
    void wr16(u32 a, u16 v);

    PhysicalBus* bus_ = nullptr;
    u32 index_ = 0;
    DbdmaStream* stream_ = nullptr;
    IrqCallback irq_cb_ = nullptr;
    void* irq_ctx_ = nullptr;

    u32 status_ = 0;
    u32 cmdptr_ = 0;
    u32 intr_sel_ = 0, br_sel_ = 0, wait_sel_ = 0;
    u32 progress_ = 0;    // bytes already moved for the current descriptor
    bool running_ = false;  // guards against re-entrant run()
};

class DbdmaController final : public MmioDevice {
public:
    static constexpr u32 kChannels = 32;
    static constexpr u32 kChannelStride = 0x80;
    static constexpr u32 kWindowSize = kChannels * kChannelStride;

    explicit DbdmaController(PhysicalBus& bus);
    DbdmaChannel& channel(u32 n) { return ch_[n & (kChannels - 1)]; }

    // MmioDevice: 32-bit little-endian registers (byte lanes handled).
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    DbdmaChannel ch_[kChannels];
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_DBDMA_H
