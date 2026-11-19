// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/ata/ata_bus.cpp — see ata_bus.h.

#include "core/devices/ata/ata_bus.h"

namespace g3 {

using namespace ata;

namespace {
constexpr u32 kSector = BlockDevice::kSectorSize;
constexpr u64 kLba28Max = 0x0FFFFFFFull;

// ATA strings: two characters per word, the first in the high byte,
// padded with spaces.
void put_string(u16* w, u32 first, u32 words, const char* s)
{
    bool end = false;
    for (u32 i = 0; i < words * 2; ++i) {
        char c = ' ';
        if (!end && s[i] != '\0')
            c = s[i];
        else
            end = true;
        u16& word = w[first + i / 2];
        if (i % 2 == 0)
            word = static_cast<u16>(static_cast<u8>(c) << 8);
        else
            word = static_cast<u16>(word | static_cast<u8>(c));
    }
}
} // namespace

AtaBus::AtaBus()
{
    for (u32 i = 0; i < sizeof buf_; ++i)
        buf_[i] = 0;
}

void AtaBus::attach(u32 device, BlockDevice* disk)
{
    Drive& d = drive_[device & 1];
    d = Drive{};
    d.disk = disk;
    if (disk) {
        d.status = DRDY | DSC;
        d.error = 0x01;  // power-on diagnostic passed
    }
    set_signature();
}

void AtaBus::attach_cdrom(u32 device, AtapiCdrom* drive)
{
    Drive& d = drive_[device & 1];
    d = Drive{};
    d.cd = drive;
    if (drive) {
        d.status = 0;    // PACKET devices: status 0 after reset (DRDY clear)
        d.error = 0x01;  // power-on diagnostic passed
    }
    set_signature();
}

// --- interrupt line -------------------------------------------------------------------

void AtaBus::raise_irq()
{
    irq_pending_ = true;
    update_irq();
}

void AtaBus::update_irq()
{
    // INTRQ is driven by the selected device, unless nIEN masks it.
    const bool line = irq_pending_ && !(control_ & NIEN) && present(selected());
    if (line != irq_line_) {
        irq_line_ = line;
        if (irq_cb_)
            irq_cb_(irq_ctx_, line);
    }
}

// --- registers ----------------------------------------------------------------------------

u8 AtaBus::read_alt_status() const
{
    if (!present(0) && !present(1))
        return 0x7F;  // nothing drives the bus; the host pulls DD7 low
    return cur().status;  // 0 when the selected device is absent
}

u8 AtaBus::read_reg(u32 reg)
{
    if (!present(0) && !present(1))
        return 0x7F;
    switch (reg) {
    case kError: return cur().error;
    case kSectorCount: return nsect_;
    case kLbaLow: return lbal_;
    case kLbaMid: return lbam_;
    case kLbaHigh: return lbah_;
    case kDevice: return device_;
    case kStatus:
        irq_pending_ = false;  // reading Status acknowledges the interrupt
        update_irq();
        return cur().status;
    default: return 0xFF;
    }
}

void AtaBus::write_reg(u32 reg, u8 value)
{
    switch (reg) {
    // Writing the taskfile replaces what the reset signature left there.
    case kFeatures: features_ = value; signature_ = false; return;
    case kSectorCount: nsect_ = value; signature_ = false; return;
    case kLbaLow: lbal_ = value; signature_ = false; return;
    case kLbaMid: lbam_ = value; signature_ = false; return;
    case kLbaHigh: lbah_ = value; signature_ = false; return;
    case kDevice:
        device_ = value;
        // Each device has its own taskfile: after a reset the newly
        // selected device shows its own signature.
        if (signature_)
            show_signature(selected());
        update_irq();  // the other device may now drive INTRQ
        return;
    case kCommand:
        if (trace_)
            trace_(trace_ctx_, CommandTrace{value, features_, nsect_, lbal_, lbam_, lbah_, device_});
        if (value == 0x90) {  // EXECUTE DEVICE DIAGNOSTIC: both devices
            if (present(0) || present(1)) {
                reset_drives();
                raise_irq();
            }
            return;
        }
        if (!present(selected()))
            return;
        irq_pending_ = false;  // writing Command acknowledges the interrupt
        update_irq();
        signature_ = false;
        if (cur().cd)
            execute_atapi(value);
        else
            execute(value);
        return;
    default: return;
    }
}

void AtaBus::write_device_control(u8 value)
{
    const bool was_reset = control_ & SRST;
    const bool reset = value & SRST;
    control_ = value;
    if (reset && !was_reset) {
        cancel_transfer();
        for (Drive& d : drive_)
            if (d.disk || d.cd)
                d.status = BSY;
        irq_pending_ = false;
    } else if (!reset && was_reset) {
        reset_drives();
    }
    update_irq();
}

void AtaBus::set_signature()
{
    device_ = 0;
    show_signature(0);
    signature_ = true;
}

void AtaBus::show_signature(u32 device)
{
    // ATA: count 1, LBA low 1, LBA mid/high 0. PACKET devices: mid 0x14,
    // high 0xEB (ATA/ATAPI-5).
    nsect_ = 1;
    lbal_ = 1;
    const bool packet = drive_[device & 1].cd != nullptr;
    lbam_ = packet ? 0x14 : 0;
    lbah_ = packet ? 0xEB : 0;
}

void AtaBus::reset_drives()
{
    cancel_transfer();
    for (Drive& d : drive_) {
        if (!d.disk && !d.cd)
            continue;
        d.status = d.cd ? 0 : DRDY | DSC;  // PACKET devices clear DRDY after reset
        d.error = 0x01;  // diagnostic code: passed
    }
    // Device 0 reports for both; an absent device 1 is not a failure.
    set_signature();
    irq_pending_ = false;
}

// --- command execution ---------------------------------------------------------------

void AtaBus::cancel_transfer()
{
    mode_ = Mode::Idle;
    remaining_ = 0;
    block_len_ = 0;
    pos_ = 0;
}

void AtaBus::complete(u8 status_extra)
{
    cancel_transfer();
    cur().status = static_cast<u8>(DRDY | DSC | status_extra);
    raise_irq();
}

void AtaBus::fail(u8 error)
{
    cancel_transfer();
    cur().error = error;
    cur().status = DRDY | DSC | ERR;
    raise_irq();
}

void AtaBus::abort_command() { fail(ABRT); }

bool AtaBus::decode_address(u64& lba)
{
    if (device_ & DEV_LBA) {
        lba = (static_cast<u64>(device_ & 0x0F) << 24) | (static_cast<u64>(lbah_) << 16) |
              (static_cast<u64>(lbam_) << 8) | lbal_;
        return true;
    }
    const Drive& d = cur();
    const u32 cyl = (static_cast<u32>(lbah_) << 8) | lbam_;
    const u32 head = device_ & 0x0F;
    const u32 sec = lbal_;
    if (d.sectors == 0 || sec == 0 || sec > d.sectors || head >= d.heads)
        return false;
    lba = (static_cast<u64>(cyl) * d.heads + head) * d.sectors + (sec - 1);
    return true;
}

void AtaBus::execute(u8 command)
{
    cancel_transfer();
    cur().error = 0;
    Drive& d = cur();
    switch (command) {
    case 0xEC:  // IDENTIFY DEVICE
        build_identify();
        mode_ = Mode::PioIn;
        remaining_ = 0;  // not a sector transfer
        block_len_ = kSector;
        pos_ = 0;
        d.status = DRDY | DSC | DRQ;
        raise_irq();
        return;
    case 0x20: case 0x21: start_read(Mode::PioIn, 1); return;       // READ SECTORS
    case 0x30: case 0x31: start_write(Mode::PioOut, 1); return;     // WRITE SECTORS
    case 0xC4:                                                       // READ MULTIPLE
        if (d.multiple == 0) { abort_command(); return; }
        start_read(Mode::PioIn, d.multiple);
        return;
    case 0xC5:                                                       // WRITE MULTIPLE
        if (d.multiple == 0) { abort_command(); return; }
        start_write(Mode::PioOut, d.multiple);
        return;
    case 0xC8: case 0xC9: start_read(Mode::DmaIn, kMaxMultiple); return;   // READ DMA
    case 0xCA: case 0xCB: start_write(Mode::DmaOut, kMaxMultiple); return; // WRITE DMA
    case 0x40: case 0x41: {                                          // READ VERIFY SECTORS
        u64 lba = 0;
        if (!decode_address(lba) || lba + sector_count_arg() > d.disk->sector_count()) {
            fail(IDNF);
            return;
        }
        complete();
        return;
    }
    case 0xC6: {                                                     // SET MULTIPLE MODE
        const u32 n = nsect_;
        if (n == 0) {
            d.multiple = 0;
            complete();
        } else if (n <= kMaxMultiple && (n & (n - 1)) == 0) {
            d.multiple = n;
            complete();
        } else {
            abort_command();
        }
        return;
    }
    case 0x91:                                                       // INITIALIZE DEVICE PARAMETERS
        if (nsect_ == 0) { abort_command(); return; }
        d.heads = static_cast<u16>((device_ & 0x0F) + 1);
        d.sectors = nsect_;
        complete();
        return;
    case 0xEF:                                                       // SET FEATURES
        switch (features_) {
        case 0x02: case 0x82:  // enable/disable write cache
        case 0x03:             // set transfer mode
        case 0x55: case 0xAA:  // disable/enable read look-ahead
        case 0x66: case 0xCC:  // disable/enable reverting to power-on defaults
            complete();
            return;
        default:
            abort_command();
            return;
        }
    case 0xE0: case 0x94:  // STANDBY IMMEDIATE
    case 0xE1: case 0x95:  // IDLE IMMEDIATE
    case 0xE2: case 0x96:  // STANDBY
    case 0xE3: case 0x97:  // IDLE
    case 0xE6: case 0x99:  // SLEEP
        complete();
        return;
    case 0xE5: case 0x98:  // CHECK POWER MODE: active or idle
        nsect_ = 0xFF;
        complete();
        return;
    case 0xE7:             // FLUSH CACHE
        if (d.disk->flush())
            complete();
        else
            abort_command();
        return;
    default:
        if (command >= 0x10 && command <= 0x1F) {  // RECALIBRATE
            complete();
            return;
        }
        if (command >= 0x70 && command <= 0x7F) {  // SEEK
            u64 lba = 0;
            if (!decode_address(lba) || lba >= d.disk->sector_count())
                fail(IDNF);
            else
                complete();
            return;
        }
        abort_command();  // includes PACKET and IDENTIFY PACKET DEVICE
        return;
    }
}

// --- sector transfers ----------------------------------------------------------------

void AtaBus::start_read(Mode mode, u32 block)
{
    u64 lba = 0;
    const u32 n = sector_count_arg();
    if (!decode_address(lba) || lba + n > cur().disk->sector_count()) {
        fail(IDNF);
        return;
    }
    lba_ = lba;
    remaining_ = n;
    block_ = block;
    mode_ = mode;
    if (!load_block()) {
        fail(UNC);
        return;
    }
    cur().status = DRDY | DSC | DRQ;
    if (mode == Mode::PioIn)
        raise_irq();          // first block ready
    else if (dma_cb_)
        dma_cb_(dma_ctx_);    // DMA may run now
}

void AtaBus::start_write(Mode mode, u32 block)
{
    u64 lba = 0;
    const u32 n = sector_count_arg();
    if (!decode_address(lba) || lba + n > cur().disk->sector_count()) {
        fail(IDNF);
        return;
    }
    if (cur().disk->read_only()) {
        abort_command();
        return;
    }
    lba_ = lba;
    remaining_ = n;
    block_ = block;
    mode_ = mode;
    block_len_ = (remaining_ < block_ ? remaining_ : block_) * kSector;
    pos_ = 0;
    cur().status = DRDY | DSC | DRQ;  // no interrupt before the first PIO block
    if (mode == Mode::DmaOut && dma_cb_)
        dma_cb_(dma_ctx_);
}

bool AtaBus::load_block()
{
    const u32 k = remaining_ < block_ ? remaining_ : block_;
    block_len_ = k * kSector;
    pos_ = 0;
    return cur().disk->read(lba_, k, buf_);
}

bool AtaBus::store_block()
{
    return cur().disk->write(lba_, block_len_ / kSector, buf_);
}

void AtaBus::after_in_block()
{
    if (remaining_ == 0) {  // IDENTIFY: single block, done
        cancel_transfer();
        cur().status = DRDY | DSC;
        return;
    }
    const u32 k = block_len_ / kSector;
    lba_ += k;
    remaining_ -= k;
    const bool dma = mode_ == Mode::DmaIn;
    if (remaining_ == 0) {
        if (dma) {
            complete();  // DMA: interrupt at the end of the command
        } else {
            cancel_transfer();  // PIO read: no interrupt after the last block
            cur().status = DRDY | DSC;
        }
        return;
    }
    if (!load_block()) {
        fail(UNC);
        return;
    }
    if (!dma)
        raise_irq();  // next PIO block ready
}

void AtaBus::after_out_block()
{
    if (!store_block()) {
        abort_command();
        return;
    }
    const u32 k = block_len_ / kSector;
    lba_ += k;
    remaining_ -= k;
    if (remaining_ == 0) {
        complete();  // interrupt after the last block is written
        return;
    }
    block_len_ = (remaining_ < block_ ? remaining_ : block_) * kSector;
    pos_ = 0;
    if (mode_ == Mode::PioOut)
        raise_irq();  // ready for the next PIO block
}

void AtaBus::read_data(u8* out, u32 len)
{
    for (u32 i = 0; i < len; ++i) {
        if (mode_ == Mode::AtapiIn) {
            out[i] = atapi_byte();
            --atapi_total_;
            if (--atapi_chunk_ == 0) {
                if (atapi_total_ == 0)
                    finish_packet();
                else
                    next_atapi_chunk();
            }
            continue;
        }
        if (mode_ != Mode::PioIn) {
            out[i] = 0xFF;  // no data phase: undefined; return all ones
            continue;
        }
        out[i] = buf_[pos_++];
        if (pos_ >= block_len_)
            after_in_block();
    }
}

void AtaBus::write_data(const u8* in, u32 len)
{
    for (u32 i = 0; i < len; ++i) {
        if (mode_ == Mode::Packet) {
            packet_[packet_pos_++] = in[i];
            if (packet_pos_ == sizeof packet_)
                run_packet();
        } else if (mode_ == Mode::AtapiOut) {
            --atapi_total_;  // parameter data: accepted and dropped
            if (--atapi_chunk_ == 0) {
                if (atapi_total_ == 0)
                    finish_packet();
                else
                    next_atapi_chunk();
            }
        } else if (mode_ == Mode::PioOut) {
            buf_[pos_++] = in[i];
            if (pos_ >= block_len_)
                after_out_block();
        } else {
            return;
        }
    }
}

u32 AtaBus::dma_read(u8* out, u32 len)
{
    u32 n = 0;
    while (n < len && mode_ == Mode::AtapiDmaIn) {
        out[n++] = atapi_byte();
        if (--atapi_total_ == 0)
            finish_packet();  // the interrupt at the end of the command
    }
    while (n < len && mode_ == Mode::DmaIn) {
        u32 chunk = block_len_ - pos_;
        if (chunk > len - n)
            chunk = len - n;
        for (u32 i = 0; i < chunk; ++i)
            out[n + i] = buf_[pos_ + i];
        n += chunk;
        pos_ += chunk;
        if (pos_ >= block_len_)
            after_in_block();
    }
    return n;
}

u32 AtaBus::dma_write(const u8* in, u32 len)
{
    u32 n = 0;
    while (n < len && mode_ == Mode::DmaOut) {
        u32 chunk = block_len_ - pos_;
        if (chunk > len - n)
            chunk = len - n;
        for (u32 i = 0; i < chunk; ++i)
            buf_[pos_ + i] = in[n + i];
        n += chunk;
        pos_ += chunk;
        if (pos_ >= block_len_)
            after_out_block();
    }
    return n;
}

// --- ATAPI ---------------------------------------------------------------------------
// ATA/ATAPI-5, PACKET protocol: the host writes PACKET (0xA0) with the
// byte count limit in LBA mid/high and DMA in Features bit 0; the device
// asks for the 12-byte packet (Interrupt Reason = sector count register:
// CoD 1, IO 0; DRQ). For data in, each DRQ block has its byte count in LBA
// mid/high, Interrupt Reason IO and an interrupt; the command ends with
// Interrupt Reason CoD|IO, DRQ clear and an interrupt.

void AtaBus::execute_atapi(u8 command)
{
    cancel_transfer();
    Drive& d = cur();
    d.error = 0;
    switch (command) {
    case 0xA0:  // PACKET
        mode_ = Mode::Packet;
        packet_pos_ = 0;
        byte_limit_ = (static_cast<u32>(lbah_) << 8) | lbam_;
        nsect_ = 0x01;  // CoD: the device wants the command packet
        d.status = DRDY | DSC | DRQ;  // no interrupt (IDENTIFY word 0: accelerated DRQ)
        return;
    case 0xA1: {  // IDENTIFY PACKET DEVICE
        u16 w[256];
        AtapiCdrom::identify(w);
        for (u32 i = 0; i < 256; ++i) {
            buf_[2 * i] = static_cast<u8>(w[i]);
            buf_[2 * i + 1] = static_cast<u8>(w[i] >> 8);
        }
        mode_ = Mode::PioIn;
        remaining_ = 0;
        block_len_ = kSector;
        pos_ = 0;
        d.status = DRDY | DSC | DRQ;
        raise_irq();
        return;
    }
    case 0xEC:  // IDENTIFY DEVICE: aborted, with the signature showing (how hosts find ATAPI)
        show_signature(selected());
        abort_command();
        return;
    case 0x08:  // DEVICE RESET: back to the signature, no interrupt
        show_signature(selected());
        signature_ = true;
        d.status = 0;
        d.error = 0x01;
        return;
    case 0xEF:  // SET FEATURES
        if (features_ == 0x03 || features_ == 0x02 || features_ == 0x82 || features_ == 0x55 ||
            features_ == 0xAA || features_ == 0x66 || features_ == 0xCC)
            complete();
        else
            abort_command();
        return;
    case 0xE5:  // CHECK POWER MODE
        nsect_ = 0xFF;
        complete();
        return;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: case 0xE7:
        complete();
        return;
    default:
        abort_command();
        return;
    }
}

void AtaBus::run_packet()
{
    const AtapiCdrom::Result r = cur().cd->command(packet_, resp_);
    cancel_transfer();
    using Kind = AtapiCdrom::Result::Kind;
    if (r.kind == Kind::Status) {
        finish_packet();
        return;
    }
    if (r.kind == Kind::DataOut) {
        mode_ = Mode::AtapiOut;
        atapi_total_ = r.length;
        next_atapi_chunk();
        return;
    }
    from_sectors_ = r.kind == Kind::SectorsIn;
    resp_pos_ = 0;
    cd_lba_ = r.lba;
    cd_pos_ = AtapiCdrom::kBlock;  // load the first block on the first byte
    atapi_total_ = from_sectors_ ? r.count * AtapiCdrom::kBlock : r.length;
    if (features_ & 0x01) {  // DMA
        mode_ = Mode::AtapiDmaIn;
        nsect_ = 0x02;
        cur().status = DRDY | DSC | DRQ;
        if (dma_cb_)
            dma_cb_(dma_ctx_);
    } else {
        mode_ = Mode::AtapiIn;
        next_atapi_chunk();
    }
}

void AtaBus::next_atapi_chunk()
{
    u32 limit = byte_limit_ & ~1u;
    if (limit == 0)
        limit = 0xFFFE;  // 0 is not a valid limit; use the largest
    atapi_chunk_ = atapi_total_ < limit ? atapi_total_ : limit;
    lbam_ = static_cast<u8>(atapi_chunk_);
    lbah_ = static_cast<u8>(atapi_chunk_ >> 8);
    nsect_ = mode_ == Mode::AtapiOut ? 0x00 : 0x02;  // IO: device to host
    cur().status = DRDY | DSC | DRQ;
    raise_irq();
}

void AtaBus::finish_packet()
{
    cancel_transfer();
    const u8 err = cur().cd->error_register();
    cur().error = err;
    nsect_ = 0x03;  // CoD | IO: status
    cur().status = static_cast<u8>(DRDY | DSC | (err ? ERR : 0));
    raise_irq();
}

u8 AtaBus::atapi_byte()
{
    if (!from_sectors_)
        return resp_pos_ < sizeof resp_ ? resp_[resp_pos_++] : 0;
    if (cd_pos_ >= AtapiCdrom::kBlock) {
        if (!cur().cd->read_block(cd_lba_, buf_))
            for (u32 i = 0; i < AtapiCdrom::kBlock; ++i)
                buf_[i] = 0;  // the disc vanished mid-transfer
        ++cd_lba_;
        cd_pos_ = 0;
    }
    return buf_[cd_pos_++];
}

// --- IDENTIFY DEVICE -------------------------------------------------------------------

void AtaBus::build_identify()
{
    const Drive& d = cur();
    u16 w[256] = {};
    const u64 total = d.disk->sector_count();
    const u64 lba28 = total < kLba28Max ? total : kLba28Max;
    u64 cyl = total / (16 * 63);
    if (cyl > 16383)
        cyl = 16383;
    w[0] = 0x0040;                          // ATA device, fixed
    w[1] = static_cast<u16>(cyl);           // default cylinders
    w[3] = 16;                              // default heads
    w[6] = 63;                              // default sectors per track
    put_string(w, 10, 10, "G3MP00000001");  // serial number
    put_string(w, 23, 4, "0.1");            // firmware revision
    put_string(w, 27, 20, "G3MacPi virtual disk");
    w[47] = static_cast<u16>(0x8000 | kMaxMultiple);
    w[49] = 0x0B00;                         // IORDY, LBA, DMA supported
    w[50] = 0x4000;
    w[51] = 0x0200;                         // PIO mode 2 timing (obsolete field)
    w[53] = 0x0007;                         // words 54-58, 64-70 and 88 valid
    u64 cur_cyl = (d.heads && d.sectors) ? total / (static_cast<u64>(d.heads) * d.sectors) : 0;
    if (cur_cyl > 65535)
        cur_cyl = 65535;
    const u32 cur_cap = static_cast<u32>(cur_cyl * d.heads * d.sectors);
    w[54] = static_cast<u16>(cur_cyl);
    w[55] = d.heads;
    w[56] = d.sectors;
    w[57] = static_cast<u16>(cur_cap);
    w[58] = static_cast<u16>(cur_cap >> 16);
    w[59] = static_cast<u16>(d.multiple ? (0x0100 | d.multiple) : 0);
    w[60] = static_cast<u16>(lba28);
    w[61] = static_cast<u16>(lba28 >> 16);
    w[63] = 0x0007;                         // multiword DMA modes 0-2 supported
    w[64] = 0x0003;                         // PIO modes 3 and 4
    w[65] = 120;                            // ns: min multiword DMA cycle
    w[66] = 120;                            // ns: recommended multiword DMA cycle
    w[67] = 120;                            // ns: min PIO cycle without IORDY
    w[68] = 120;                            // ns: min PIO cycle with IORDY
    w[80] = 0x003E;                         // ATA-1 to ATA-5
    w[82] = 0x0068;                         // look-ahead, write cache, power management
    w[83] = 0x5000;                         // FLUSH CACHE
    w[84] = 0x4000;
    w[85] = 0x0068;
    w[86] = 0x1000;
    w[87] = 0x4000;
    for (u32 i = 0; i < 256; ++i) {
        buf_[2 * i] = static_cast<u8>(w[i]);
        buf_[2 * i + 1] = static_cast<u8>(w[i] >> 8);
    }
}

} // namespace g3
