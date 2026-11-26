// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/heathrow_pic.h — interrupt controller of the
// Heathrow-family mac-io chips (Heathrow, Paddington — the Blue & White G3
// uses Paddington).
//
// Register model (little-endian 32-bit registers), established from two
// independent descriptions that agree (docs/hardware/heathrow-pic.md):
//   window offset 0x10: bank for IRQs 32-63
//   window offset 0x20: bank for IRQs 0-31
//   bank + 0x0  event   read-only: latched edge events
//   bank + 0x4  enable  read/write: 1 = source enabled
//   bank + 0x8  ack     write-only: writing 1 clears latched edge events
//   bank + 0xC  level   read-only: current state of the input lines
// The CPU interrupt output is asserted while
//   (event | (level & level_triggered)) & enable
// is non-zero in either bank.
//
// Interrupt mode (from DingusPPC's MacIoTwo model, an emulator that boots
// Mac OS on these machines; to verify): bit 31 of the IRQ 0-31 enable
// register is not a source but IntMode, 0 = native, 1 = "68K-style". It
// reads back as bit 31 of the IRQ 32-63 enable register too (read-only
// there). In 68K mode an edge input latches on both transitions, and
// writing bit 31 to the IRQ 0-31 ack register clears every latched event.
// Mac OS selects 68K mode and acknowledges that way.
//
// Which inputs are level- rather than edge-triggered is a property of the
// machine's wiring; it is configured by the machine model
// (set_level_triggered) — the Blue & White values are NOT yet verified.

#ifndef G3_CORE_DEVICES_MACIO_HEATHROW_PIC_H
#define G3_CORE_DEVICES_MACIO_HEATHROW_PIC_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class HeathrowPic final : public MmioDevice {
public:
    static constexpr u32 kNumInputs = 64;
    static constexpr u32 kWindowSize = 0x40;
    using OutputCallback = void (*)(void* context, bool asserted);

    void set_output(OutputCallback cb, void* context);
    // Bit n = IRQ n is level-triggered.
    void set_level_triggered(u64 mask);

    // Device side: drive input line 'irq' (0..63).
    void set_input(u32 irq, bool asserted);

    bool output() const { return output_; }
    u64 enabled_mask() const { return enable_; }
    bool mode_68k() const { return (enable_ & kModeBit) != 0; }
    static constexpr u64 kModeBit = 1ull << 31;  // in the IRQ 0-31 enable register
    // DMA interrupt sources (IRQs 0-10, DBDMA channels): a DMA channel only
    // raises its line; software lowers it by writing the source's bit to the
    // IRQ 0-31 ack register (DingusPPC MacIoTwo/GrandCentral clear_dma_int;
    // to verify). Mac OS's nanokernel acks with bit 31 and then dispatches
    // from the level register, so a DMA interrupt that dropped by itself
    // was never seen. docs/hardware/heathrow-pic.md.
    // Also IRQs 32 and 33, the Ethernet DMA channels (DingusPPC registers
    // them as DMA interrupts, DMA_ETHERNET_Tx/Rx); acked in the IRQ 32-63 bank.
    static constexpr u64 kDmaSources = 0x7FFull | (3ull << 32);

    // The chip's other registers in this window (Heathrow/Paddington, per
    // DingusPPC's macio.h and Linux's feature.c): 0x34 MBCR / ID, 0x38 the
    // feature control register (FCR), 0x3C auxiliary control. They are kept
    // as written: Mac OS's Ethernet driver (lanLib) switches the BMAC cell
    // on through FCR bits 29-31 (enable, transceiver enable, reset; DingusPPC
    // MIO_OH_FC_ETH_*) and reads them back, giving up when they read 0 (seen
    // in a trace). No side effects are modelled.
    static constexpr u32 kFcrOffset = 0x38;
    u32 fcr() const { return misc_[(kFcrOffset - 0x30) >> 2]; }

    // MmioDevice
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    // Bank 0 = IRQs 0-31 (offset 0x20), bank 1 = IRQs 32-63 (offset 0x10).
    static int bank_for_offset(u32 offset);
    u32 bank_bits(u64 v, int bank) const { return static_cast<u32>(v >> (32 * bank)); }
    void update_output();

    u32 misc_[4] = {};  // 0x30, 0x34, 0x38 (FCR), 0x3C
    u64 events_ = 0;
    u64 enable_ = 0;
    u64 levels_ = 0;
    u64 level_triggered_ = 0;
    bool output_ = false;
    OutputCallback cb_ = nullptr;
    void* ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_HEATHROW_PIC_H
