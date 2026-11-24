// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/bmac.h — the Blue & White G3's built-in Ethernet:
// the "BMAC+" cell of Paddington, with its frames on two DBDMA channels.
//
// Facts (docs/hardware/bmac.md):
//  - Registers at mac-io + 0x11000 (DingusPPC MacIoTwo); 16-bit,
//    little-endian (Linux bmac.c: in_le16/out_le16 at the byte offsets of
//    bmac.h). Among them: XIFC 0x000, TXFIFOCSR 0x100, RXFIFOCSR 0x120,
//    XCVRIF 0x160, CHIPID 0x170, MIFCSR 0x180, SROMCSR 0x190, STATUS
//    0x200 (reading clears it), INTDISABLE 0x210, TXRST 0x420, TXCFG 0x430,
//    RXRST 0x620, RXCFG 0x630, MADD2/1/0 0x660/0x670/0x680, BHASH3-0
//    0x700-0x730.
//  - STATUS / INTDISABLE bits: FrameReceived 0x0001, FrameSent 0x0100.
//  - TXCFG bit 0 enables the transmitter, RXCFG bit 0 the receiver; RXCFG
//    0x0040 promiscuous, 0x0100 keep the CRC, 0x0200 reject own packets,
//    0x0800 hash filter (Linux bmac.h).
//  - CHIPID reads 0xC7 on Paddington (DingusPPC BigMac).
//  - MIFCSR bit-bangs MII to the PHY: bit 0 clock, 1 data out, 2 drive
//    enable, 3 data in (DingusPPC; Linux's bit patterns agree). The frames
//    are IEEE 802.3 clause 22 management frames; PHY ID1 0x7810, an LXT970
//    (DingusPPC).
//  - DMA: transmit on the channel at mac-io + 0x8200, receive at + 0x8300;
//    interrupts 42 (BMAC), 32 (tx DMA), 33 (rx DMA) — a real B&W's Linux log
//    lists 42, 32, 33 and DingusPPC has the same.
//  - Linux receives into INPUT_LAST descriptors and takes the length from
//    the residual count, with the 4-byte CRC kept (RXCFG 0x0100).
//
// Not modelled: the serial EEPROM (SROMCSR reads 0; the address comes from
// the device tree's local-mac-address), collisions and counters, and the
// multicast hash (with the hash filter on, all multicast frames are taken).

#ifndef G3_CORE_DEVICES_MACIO_BMAC_H
#define G3_CORE_DEVICES_MACIO_BMAC_H

#include "core/common/types.h"
#include "core/devices/macio/dbdma.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"

namespace g3 {

// The wire: whole Ethernet frames without the CRC.
class NetLink {
public:
    static constexpr u32 kMaxFrame = 1518;
    virtual void net_send(const u8* frame, u32 len) = 0;
    // A received frame into buf (kMaxFrame bytes); 0 when none is waiting.
    virtual u32 net_receive(u8* buf) = 0;

protected:
    ~NetLink() = default;
};

class MacioBmac final : public MmioDevice {
public:
    static constexpr u32 kWindowSize = 0x1000;
    enum Reg : u32 {
        XIFC = 0x000, TXFIFOCSR = 0x100, RXFIFOCSR = 0x120, XCVRIF = 0x160, CHIPID = 0x170, MIFCSR = 0x180,
        SROMCSR = 0x190, STATUS = 0x200, INTDISABLE = 0x210, TXRST = 0x420, TXCFG = 0x430, RXRST = 0x620,
        RXCFG = 0x630, MADD2 = 0x660, MADD1 = 0x670, MADD0 = 0x680, BHASH3 = 0x700, BHASH0 = 0x730,
    };
    enum : u16 {
        FrameReceived = 0x0001, FrameSent = 0x0100,
        TxMACEnable = 0x0001, RxMACEnable = 0x0001, RxPromisc = 0x0040, RxCRCNoStrip = 0x0100,
        RxRejectOwn = 0x0200, RxHashFilter = 0x0800, LinkStatus = 0x0100,
    };
    static constexpr u16 kChipId = 0xC7;
    static constexpr u64 kPollPs = 500000000ull;  // look for received frames every 0.5 ms

    explicit MacioBmac(Scheduler& scheduler);

    using IrqCallback = void (*)(void* context, bool asserted);
    void set_irq(IrqCallback cb, void* ctx) { irq_ = cb; irq_ctx_ = ctx; }
    void attach_dma(DbdmaChannel* tx, DbdmaChannel* rx);
    void set_link(NetLink* link) { link_ = link; }

    void station_address(u8 out[6]) const;
    u64 frames_sent() const { return sent_; }
    u64 frames_received() const { return received_; }
    u64 frames_dropped() const { return dropped_; }

    // MmioDevice.
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    struct Tx final : DbdmaStream {
        MacioBmac* m = nullptr;
        u32 dma_output(const u8* data, u32 len, bool last) override;
        u32 dma_input(u8*, u32) override { return 0; }
    };
    struct Rx final : DbdmaStream {
        MacioBmac* m = nullptr;
        u32 dma_output(const u8*, u32, bool) override { return 0; }
        u32 dma_input(u8* data, u32 len) override;
        bool dma_input_packet_end() override;
    };
    // The PHY on the MII management bus (address 0).
    struct Phy {
        enum State : u8 { Idle, Start, Header, ReadOut, WriteIn };
        u16 reg[32] = {};
        State state = Idle;
        u32 bits = 0, n = 0;
        u32 out = 0;   // bits still to send, MSB first, n of them
        bool mdi = true;
        Phy();
        void clock(bool driven, bool bit);
    };

    u16 read_reg(u32 reg);
    void write_reg(u32 reg, u16 value);
    void update_irq();
    void raise(u16 bits);
    bool accept(const u8* f, u32 len) const;
    void poll();
    void start_poll();
    static void poll_cb(void* c, u64) { static_cast<MacioBmac*>(c)->poll_tick(); }
    void poll_tick();

    Scheduler& sched_;
    NetLink* link_ = nullptr;
    DbdmaChannel* tx_ch_ = nullptr;
    DbdmaChannel* rx_ch_ = nullptr;
    Tx tx_;
    Rx rx_;
    Phy phy_;
    u16 regs_[0x80] = {};  // by offset / 0x10
    u16 status_ = 0;
    IrqCallback irq_ = nullptr;
    void* irq_ctx_ = nullptr;
    bool line_ = false;
    bool polling_ = false;

    u8 txbuf_[2048] = {};
    u32 tx_len_ = 0;
    u8 rxbuf_[NetLink::kMaxFrame + 64] = {};
    u32 rx_len_ = 0, rx_pos_ = 0;  // rx_len_ 0: no frame waiting
    u64 sent_ = 0, received_ = 0, dropped_ = 0;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_BMAC_H
