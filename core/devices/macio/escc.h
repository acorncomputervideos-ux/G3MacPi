// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/escc.h — mac-io's ESCC (Zilog Z85C30 serial controller).
// A device can be attached to a channel (the virtual modem, on A): it gets
// the bytes the Mac sends and the lines it drives (DTR, RTS), and feeds the
// receiver and the DCD/CTS inputs. Without one, nothing is received.
//
// Facts (docs/hardware/escc.md):
//  - Z8530 register access (Linux drivers/tty/serial/pmac_zilog.h, read for
//    facts): a write to the control port selects a register in WR0's low 3
//    bits (+8 with the "point high" command, WR0 bits 3-5 = 001); the next
//    control access reads RR n or writes WR n, then the pointer returns to 0.
//    WR0 commands: 2 reset ext/status interrupts, 5 reset Tx interrupt
//    pending, 6 error reset, 7 reset highest IUS. WR1 bit 1 = Tx interrupt
//    enable. WR9 (one register for both channels): 0x80 reset channel A,
//    0x40 reset channel B, 0xC0 hardware reset, 0x08 master interrupt enable.
//    RR0: 0x01 Rx available, 0x04 Tx buffer empty, 0x08 DCD, 0x20 CTS, 0x40
//    Tx underrun/EOM. RR1: 0x01 all sent. RR2: the vector (WR2). RR3 (channel
//    A only): 0x02/0x10 Tx interrupt pending for B/A.
//  - MacRISC layout (pmac_zilog: control at the channel's base, data at
//    +0x10): channel B at mac-io + 0x13000, channel A at + 0x13020 (Linux on a
//    real B&W: ttyPZ0 at 0x80813020, ttyPZ1 at 0x80813000).
//  - Legacy layout at mac-io + 0x12000 (Apple's "escc-legacy"): the 68K Mac
//    SCC arrangement, B control +0, A control +2, B data +4, A data +6
//    (to verify for Paddington).
//  - Receiving (pmac_zilog.h names): RR0 Rx_CH_AV 0x01; RR1 Rx_OVR 0x20 (a
//    character arrived with the FIFO full); WR3 RxENABLE 0x01; WR1 Rx
//    interrupt mode (RxINT_MASK 0x18): 0x08 on the first character (re-armed
//    by WR0 command RES_RxINT_FC 0x20) or a special condition, 0x10 on every
//    character, 0x18 special conditions only. The Z85C30 holds 3 received
//    characters.
//  - Modem lines: WR5 DTR 0x80, RTS 0x02 (outputs); RR0 DCD 0x08, CTS 0x20
//    (inputs). A change of DCD or CTS raises the external/status interrupt
//    if WR1 EXT_INT_ENAB 0x01 and its WR15 enable (DCDIE 0x08, CTSIE 0x20)
//    are set; WR0 command RES_EXT_INT (0x10) clears it.
//  - RR3 (channel A): CHBEXT 0x01, CHBTxIP 0x02, CHBRxIP 0x04, CHAEXT 0x08,
//    CHATxIP 0x10, CHARxIP 0x20.
//  - Baud rate (WR12/13 time constant TC, WR4 clock mode x1/x16/x32/x64):
//    PCLK / (2 * mode * (TC + 2)), with the ESCC clock 3.6864 MHz (QEMU's
//    mac-io model, ESCC_CLOCK).
// Interrupts: channel A on PIC input 15, B on 16 (the real B&W's Linux log),
// asserted while the channel has a pending interrupt and WR9's MIE is set.
// Not modelled: DMA, sync/SDLC modes, parity and framing errors, the
// status in RR2's vector, interrupt priority daisy chains (IUS).

#ifndef G3_CORE_DEVICES_MACIO_ESCC_H
#define G3_CORE_DEVICES_MACIO_ESCC_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class MacioEscc final : public MmioDevice {
public:
    // Mapped at mac-io + 0x12000: legacy view at +0, MacRISC view at +0x1000.
    static constexpr u32 kWindowSize = 0x2000;
    enum Channel : u32 { A = 0, B = 1 };

    using IrqCallback = void (*)(void* context, u32 channel, bool asserted);
    void set_irq(IrqCallback cb, void* ctx) { irq_ = cb; irq_ctx_ = ctx; }
    using TxSink = void (*)(void* context, u32 channel, u8 byte);
    void set_tx_sink(TxSink s, void* ctx) { tx_ = s; tx_ctx_ = ctx; }
    // What the Mac drives: called when DTR or RTS changes.
    using LinesCallback = void (*)(void* context, u32 channel, bool dtr, bool rts);
    void set_lines_callback(LinesCallback cb, void* ctx) { lines_ = cb; lines_ctx_ = ctx; }

    // The attached device's side.
    static constexpr u32 kRxFifo = 3;
    bool rx_ready(u32 ch) const;          // receiver enabled and room for a character
    void rx_push(u32 ch, u8 byte);        // with the FIFO full: lost, RR1 overrun set
    void set_dcd(u32 ch, bool on);
    void set_cts(u32 ch, bool on);
    bool dtr(u32 ch) const { return (chan_[ch & 1].wr[5] & 0x80) != 0; }
    bool rts(u32 ch) const { return (chan_[ch & 1].wr[5] & 0x02) != 0; }
    u32 baud(u32 ch) const;               // 0 if not set up
    static constexpr u32 kClockHz = 3686400;

    u8 read_control(u32 ch);
    void write_control(u32 ch, u8 v);
    u8 read_data(u32 ch);
    void write_data(u32 ch, u8 v);
    u8 wr(u32 ch, u32 n) const { return chan_[ch & 1].wr[n & 15]; }

    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    struct Chan {
        u8 wr[16] = {};
        u32 ptr = 0;
        bool tx_ip = false;   // Tx interrupt pending
        bool ext_ip = false;  // external/status interrupt pending
        bool rx_first = true; // "first character" mode: the next one interrupts
        bool rx_ip = false;   // Rx interrupt pending
        u8 fifo[kRxFifo] = {};
        u32 fifo_n = 0;
        bool overrun = false;
        bool dcd = false, cts = false;
        bool lines_dtr = false, lines_rts = false;  // last reported
    };
    void update_rx_ip(u32 ch);
    void report_lines(u32 ch);
    void reset_channel(u32 ch);
    u8 rr(u32 ch, u32 n) const;
    void update_irq();
    // Decodes an offset in the window: channel, true = data port. False if
    // the offset isn't a port.
    static bool decode(u32 offset, u32& ch, bool& data);

    Chan chan_[2];
    u8 wr9_ = 0;
    bool line_[2] = {};
    IrqCallback irq_ = nullptr;
    void* irq_ctx_ = nullptr;
    TxSink tx_ = nullptr;
    void* tx_ctx_ = nullptr;
    LinesCallback lines_ = nullptr;
    void* lines_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_ESCC_H
