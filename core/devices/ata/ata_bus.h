// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/ata/ata_bus.h — one ATA (IDE) channel with up to two hard
// disks (device 0 "master", device 1 "slave"), independent of the host
// controller that exposes its registers.
//
// Facts (docs/hardware/ata.md; ATA/ATAPI-5 and -6 standards, T13):
//  - Command block registers 0-7: data, error/features, sector count,
//    LBA low/sector, LBA mid/cylinder low, LBA high/cylinder high,
//    device/head, status/command. Control block: alternate status /
//    device control.
//  - Status: BSY 0x80, DRDY 0x40, DF 0x20, DSC 0x10, DRQ 0x08, ERR 0x01.
//    Error: ABRT 0x04, IDNF 0x10, UNC 0x40. Device control: SRST 0x04,
//    nIEN 0x02. Device register: LBA 0x40, DEV 0x10.
//  - Reading Status clears a pending interrupt; Alternate Status doesn't.
//  - Writes to the command block go to both devices; only the selected
//    device executes commands and drives the status.
//  - Signature after reset/diagnostic for an ATA device: sector count 1,
//    LBA low 1, LBA mid 0, LBA high 0, device 0.
//  - A sector count of 0 means 256 sectors (28-bit commands).
//
// Data moves as a byte stream in device order: stream byte 2n is the low
// byte of data word n (how IDENTIFY words and sectors are laid out).
// Commands complete in zero emulated time.

#ifndef G3_CORE_DEVICES_ATA_ATA_BUS_H
#define G3_CORE_DEVICES_ATA_ATA_BUS_H

#include "core/common/types.h"
#include "core/storage/block_device.h"

namespace g3 {

namespace ata {
// Status register.
constexpr u8 BSY = 0x80, DRDY = 0x40, DF = 0x20, DSC = 0x10, DRQ = 0x08, ERR = 0x01;
// Error register.
constexpr u8 ABRT = 0x04, IDNF = 0x10, UNC = 0x40;
// Device control register.
constexpr u8 SRST = 0x04, NIEN = 0x02;
// Device register.
constexpr u8 DEV_LBA = 0x40, DEV_SLAVE = 0x10;
// Command block register indices.
enum Reg : u8 { kData = 0, kError = 1, kFeatures = 1, kSectorCount = 2, kLbaLow = 3,
                kLbaMid = 4, kLbaHigh = 5, kDevice = 6, kStatus = 7, kCommand = 7 };
} // namespace ata

class AtaBus {
public:
    using IrqCallback = void (*)(void* context, bool asserted);
    using DmaCallback = void (*)(void* context);
    static constexpr u32 kMaxMultiple = 16;  // sectors per DRQ block (READ/WRITE MULTIPLE)

    AtaBus();

    // Attach a disk image as device 0 or 1 (nullptr detaches).
    void attach(u32 device, BlockDevice* disk);
    void set_irq(IrqCallback cb, void* context) { irq_cb_ = cb; irq_ctx_ = context; }
    // Called when a DMA command is ready to move data (the host
    // controller should then let its DMA engine run).
    void set_dma_request(DmaCallback cb, void* context) { dma_cb_ = cb; dma_ctx_ = context; }

    // Host side: command block registers 1-7 and the control block.
    u8 read_reg(u32 reg);
    void write_reg(u32 reg, u8 value);
    u8 read_alt_status() const;
    void write_device_control(u8 value);

    // Host side: PIO data register (byte stream; 2 or 4 bytes per access).
    void read_data(u8* out, u32 len);
    void write_data(const u8* in, u32 len);

    // Host side: DMA. Return the number of bytes moved; fewer than asked
    // means no (more) DMA data is pending right now.
    u32 dma_read(u8* out, u32 len);
    u32 dma_write(const u8* in, u32 len);
    bool dma_pending() const { return mode_ == Mode::DmaIn || mode_ == Mode::DmaOut; }

    // Diagnostics: called for every command written to the Command
    // register, with the taskfile as written (before the command runs).
    struct CommandTrace {
        u8 command, features, count, lba_low, lba_mid, lba_high, device;
    };
    using CommandTraceFn = void (*)(void* context, const CommandTrace& t);
    void set_command_trace(CommandTraceFn fn, void* context) { trace_ = fn; trace_ctx_ = context; }

    bool irq_asserted() const { return irq_line_; }
    u32 selected() const { return (device_ & ata::DEV_SLAVE) ? 1 : 0; }

private:
    enum class Mode : u8 { Idle, PioIn, PioOut, DmaIn, DmaOut };

    struct Drive {
        BlockDevice* disk = nullptr;
        u8 status = 0;
        u8 error = 0;
        u16 heads = 16, sectors = 63;  // current CHS translation
        u32 multiple = 0;              // 0 = READ/WRITE MULTIPLE disabled
    };

    Drive& cur() { return drive_[selected()]; }
    const Drive& cur() const { return drive_[selected()]; }
    bool present(u32 d) const { return drive_[d].disk != nullptr; }

    void reset_drives();
    void set_signature();
    void execute(u8 command);
    void complete(u8 status_extra = 0);
    void abort_command();
    void fail(u8 error);
    void raise_irq();
    void update_irq();
    bool decode_address(u64& lba);
    u32 sector_count_arg() const { return nsect_ ? nsect_ : 256u; }
    void build_identify();
    void start_read(Mode mode, u32 block);
    void start_write(Mode mode, u32 block);
    bool load_block();   // reads the next block of sectors into the buffer
    bool store_block();  // writes the buffer to the disk
    void after_in_block();
    void after_out_block();
    void cancel_transfer();

    Drive drive_[2];
    // Command block (written to both devices).
    u8 features_ = 0, nsect_ = 0, lbal_ = 0, lbam_ = 0, lbah_ = 0, device_ = 0;
    u8 control_ = 0;

    // Transfer state for the selected device's current command.
    Mode mode_ = Mode::Idle;
    u64 lba_ = 0;           // next sector to read or write
    u32 remaining_ = 0;     // sectors still to transfer (including the buffered block)
    u32 block_ = 1;         // sectors per DRQ block
    u32 block_len_ = 0;     // bytes in the current block
    u32 pos_ = 0;           // byte position inside the buffer
    u8 buf_[kMaxMultiple * BlockDevice::kSectorSize];

    bool irq_pending_ = false;
    bool irq_line_ = false;
    IrqCallback irq_cb_ = nullptr;
    void* irq_ctx_ = nullptr;
    DmaCallback dma_cb_ = nullptr;
    void* dma_ctx_ = nullptr;
    CommandTraceFn trace_ = nullptr;
    void* trace_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_DEVICES_ATA_ATA_BUS_H
