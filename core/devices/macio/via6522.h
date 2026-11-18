// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/via6522.h — the VIA (6522-compatible Versatile
// Interface Adapter) cell in the mac-io chip, through which the CPU talks to
// the Cuda microcontroller.
//
// Facts (docs/hardware/via-cuda.md): 16 registers spaced 0x200 bytes apart
// (B, A, DIRB, DIRA, T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, SR, ACR, PCR, IFR,
// IER, ANH); timers count at 4.7 MHz / 6 (~783.333 kHz) on Power Macs
// (Linux powermac/time.c and QEMU agree). Registers are 8-bit.
//
// Behaviour follows the 6522 data sheet for the parts the Mac uses: port B
// with direction register, timer 1 (one-shot or free-running), timer 2
// (one-shot), shift register driven by the external device (the Cuda), and
// the IFR/IER interrupt logic. Simplifications are listed in the doc.

#ifndef G3_CORE_DEVICES_MACIO_VIA6522_H
#define G3_CORE_DEVICES_MACIO_VIA6522_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"

namespace g3 {

// The device on the far side of port B / the shift register (the Cuda).
class ViaPeripheral {
public:
    // Port B outputs changed: 'value' = levels of the pins the CPU drives
    // (ORB & DDRB), 'driven' = DDRB.
    virtual void via_port_b_changed(u8 value, u8 driven) = 0;
    // The CPU wrote / read the shift register.
    virtual void via_sr_written(u8 value) = 0;
    virtual void via_sr_read() = 0;

protected:
    ~ViaPeripheral() = default;
};

class Via6522 final : public MmioDevice {
public:
    static constexpr u32 kRegisterStride = 0x200;
    // Observer of register accesses through MMIO (debugging tools); null = none.
    using TraceHook = void (*)(void* context, u32 reg, u8 value, bool write);
    void set_trace(TraceHook h, void* ctx) { trace_ = h; trace_ctx_ = ctx; }
    static constexpr u32 kWindowSize = 0x2000;
    // Timer clock: 4.7 MHz / 6.
    static constexpr u64 kClockNumerator = 4700000;   // Hz * 6
    static constexpr u64 kClockDenominator = 6;

    enum Reg : u32 { B = 0, A, DIRB, DIRA, T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, SR, ACR, PCR, IFR, IER, ANH };
    enum IrqBit : u8 { CA2 = 0x01, CA1 = 0x02, SR_INT = 0x04, CB2 = 0x08, CB1 = 0x10, T2 = 0x20, T1 = 0x40 };

    using IrqCallback = void (*)(void* context, bool asserted);

    explicit Via6522(Scheduler& scheduler);

    void set_irq_output(IrqCallback cb, void* context);
    void attach_peripheral(ViaPeripheral* p) { peripheral_ = p; }

    // Peripheral side.
    void set_port_b_input(u8 mask, u8 levels);   // drive input pins
    void set_shift_register(u8 value) { sr_ = value; }
    // The peripheral's view of the shift register (no CPU-side effects).
    u8 shift_register() const { return sr_; }
    void raise_interrupt(u8 bits);               // e.g. SR_INT when a byte has shifted

    // Inspection.
    u8 acr() const { return acr_; }
    u8 port_b_output() const { return orb_ & ddrb_; }
    u8 ddrb() const { return ddrb_; }
    bool irq() const { return irq_; }

    // MmioDevice (byte registers; wider accesses use the lowest byte).
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    TraceHook trace_ = nullptr;
    void* trace_ctx_ = nullptr;
    struct Timer {
        bool running = false;
        u64 start_tick = 0;   // VIA tick at which 'start_value' was loaded
        u16 start_value = 0;
        Scheduler::Handle event = Scheduler::kInvalidHandle;
    };

    u64 now_tick() const;
    u64 tick_to_ps(u64 tick) const;
    u16 counter(const Timer& t) const;
    void start_timer(Timer& t, u16 value, bool is_t1);
    static void t1_expired(void* ctx, u64 now);
    static void t2_expired(void* ctx, u64 now);
    void update_irq();

    u8 read_reg(u32 r);
    void write_reg(u32 r, u8 v);

    Scheduler& sched_;
    ViaPeripheral* peripheral_ = nullptr;
    IrqCallback irq_cb_ = nullptr;
    void* irq_ctx_ = nullptr;
    bool irq_ = false;

    u8 orb_ = 0, ora_ = 0, ddrb_ = 0, ddra_ = 0;
    u8 pins_b_ = 0xFF;  // input levels (pulled up)
    u8 acr_ = 0, pcr_ = 0, sr_ = 0;
    u8 ifr_ = 0, ier_ = 0;
    u16 t1_latch_ = 0;
    u8 t2_latch_lo_ = 0;
    Timer t1_, t2_;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_VIA6522_H
