// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/ata/atapi_cdrom.h — an ATAPI CD-ROM drive playing a CD
// image (an ISO file of 2048-byte sectors): the packet-command side. The
// ATA bus (ata_bus.h) carries the packets and the data.
//
// Facts (docs/hardware/atapi-cdrom.md):
//  - ATA/ATAPI-5: PACKET devices; signature LBA mid 0x14, LBA high 0xEB;
//    IDENTIFY PACKET DEVICE word 0 = 0x85C0 (ATAPI, CD-ROM, removable,
//    12-byte packets); errors put the sense key in the Error register's
//    upper four bits, with ABRT.
//  - SPC/MMC (as SFF-8020i uses them): TEST UNIT READY 0x00, REQUEST SENSE
//    0x03 (fixed format, 18 bytes), INQUIRY 0x12 (peripheral type 5,
//    removable), START STOP UNIT 0x1B, PREVENT ALLOW MEDIUM REMOVAL 0x1E,
//    READ CAPACITY 0x25 (last LBA, block length 2048), READ(10) 0x28,
//    READ(12) 0xA8, READ CD 0xBE (user data only), READ TOC 0x43 (formats
//    0 and 1, LBA or MSF; the format may also come from byte 9's top bits,
//    as Apple's CD driver sends it), MODE SENSE(6/10) 0x1A/0x5A, MODE
//    SELECT(10) 0x55, SET CD SPEED 0xBB, GET CONFIGURATION 0x46.
//  - Sense: no medium 2/3A/00, medium changed 6/28/00, invalid command
//    5/20/00, invalid field 5/24/00, LBA out of range 5/21/00, removal
//    prevented 5/53/02.
//  - Apple's CD driver asks for mode page 0x31, which Apple drives answer
//    with ".App" (DingusPPC's model; to verify).

#ifndef G3_CORE_DEVICES_ATA_ATAPI_CDROM_H
#define G3_CORE_DEVICES_ATA_ATAPI_CDROM_H

#include "core/common/types.h"
#include "core/storage/block_device.h"

namespace g3 {

class AtapiCdrom {
public:
    static constexpr u32 kBlock = 2048;        // CD-ROM mode 1 user data
    static constexpr u32 kResponseMax = 256;   // largest non-sector response
    using EjectCallback = void (*)(void* context);

    // Insert (or, with nullptr, remove) a disc. An inserted disc reports
    // "medium may have changed" once, as a real drive does.
    void insert(BlockDevice* image);
    BlockDevice* image() const { return image_; }
    bool locked() const { return locked_; }
    u32 blocks() const;
    // Called when the guest ejects the disc (START STOP UNIT).
    void set_eject_callback(EjectCallback cb, void* context) { eject_cb_ = cb; eject_ctx_ = context; }

    // The outcome of a packet command.
    struct Result {
        enum class Kind : u8 { Status, DataIn, SectorsIn, DataOut } kind = Kind::Status;
        u32 length = 0;     // DataIn: bytes in the response; DataOut: bytes to accept
        u32 lba = 0, count = 0;  // SectorsIn
        bool error = false; // CHECK CONDITION (see error_register())
    };
    // Runs a 12-byte packet; non-sector data goes to `response`
    // (kResponseMax bytes).
    Result command(const u8* cdb, u8* response);
    // The ATA Error register after the last command: sense key << 4 | ABRT
    // on CHECK CONDITION, else 0.
    u8 error_register() const { return error_reg_; }
    // One 2048-byte block of the disc.
    bool read_block(u32 lba, u8* out);

    // Diagnostics: called after each packet with its 12 bytes and the
    // resulting sense (key << 16 | ASC << 8 | ASCQ; 0 = good).
    using PacketTrace = void (*)(void* context, const u8* cdb, u32 sense);
    void set_packet_trace(PacketTrace cb, void* context) { trace_ = cb; trace_ctx_ = context; }

    // IDENTIFY PACKET DEVICE data (256 words).
    static void identify(u16* words);

private:
    Result check(u8 key, u8 asc, u8 ascq = 0);
    Result good() { error_reg_ = 0; return Result{}; }
    Result data(u32 len, u32 alloc);
    bool ready(Result& r);  // medium present, no pending unit attention
    u32 mode_sense(const u8* cdb, u8* out, bool ten, bool& ok);

    BlockDevice* image_ = nullptr;
    bool unit_attention_ = false;
    bool locked_ = false;
    u8 sense_key_ = 0, asc_ = 0, ascq_ = 0;
    u8 error_reg_ = 0;
    EjectCallback eject_cb_ = nullptr;
    void* eject_ctx_ = nullptr;
    PacketTrace trace_ = nullptr;
    void* trace_ctx_ = nullptr;
    Result run(const u8* cdb, u8* response);
};

} // namespace g3

#endif // G3_CORE_DEVICES_ATA_ATAPI_CDROM_H
