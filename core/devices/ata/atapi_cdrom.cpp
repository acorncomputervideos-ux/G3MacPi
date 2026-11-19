// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/ata/atapi_cdrom.cpp — see atapi_cdrom.h.

#include "core/devices/ata/atapi_cdrom.h"

namespace g3 {

namespace {

constexpr u8 kAbrt = 0x04;
constexpr u32 kSector = BlockDevice::kSectorSize;
constexpr u32 kPerBlock = AtapiCdrom::kBlock / kSector;  // 512-byte sectors per CD block

// Sense keys and codes (SPC / MMC).
constexpr u8 kNotReady = 0x2, kIllegalRequest = 0x5, kUnitAttention = 0x6;
constexpr u8 kAscNoMedium = 0x3A, kAscMediumChanged = 0x28, kAscInvalidCommand = 0x20,
             kAscInvalidField = 0x24, kAscLbaOutOfRange = 0x21, kAscRemovalPrevented = 0x53;

constexpr u8 kTrackAdrCtrl = 0x14;  // ADR 1 (position), CTRL 4 (data track)
constexpr u8 kLeadOut = 0xAA;

u32 be16(const u8* p) { return (static_cast<u32>(p[0]) << 8) | p[1]; }
u32 be24(const u8* p) { return (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2]; }
u32 be32(const u8* p) { return (be16(p) << 16) | be16(p + 2); }
void put16(u8* p, u32 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
void put32(u8* p, u32 v) { put16(p, v >> 16); put16(p + 2, v); }

void put_text(u8* p, u32 n, const char* s)
{
    bool end = false;
    for (u32 i = 0; i < n; ++i) {
        if (!end && !s[i])
            end = true;
        p[i] = end ? ' ' : static_cast<u8>(s[i]);
    }
}

// CD address: LBA -> minutes/seconds/frames, with the 2-second (150-frame) pregap.
void put_address(u8* p, u32 lba, bool msf)
{
    if (!msf) {
        put32(p, lba);
        return;
    }
    const u32 f = lba + 150;
    p[0] = 0;
    p[1] = static_cast<u8>(f / (75 * 60));
    p[2] = static_cast<u8>((f / 75) % 60);
    p[3] = static_cast<u8>(f % 75);
}

void put_words(u16* w, u32 first, u32 words, const char* s)
{
    bool end = false;
    for (u32 i = 0; i < words * 2; ++i) {
        char c = ' ';
        if (!end && s[i])
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

void AtapiCdrom::insert(BlockDevice* image)
{
    image_ = image;
    if (image)
        unit_attention_ = true;  // "not ready to ready change, medium may have changed"
    else
        locked_ = false;
}

u32 AtapiCdrom::blocks() const
{
    if (!image_)
        return 0;
    const u64 b = image_->sector_count() / kPerBlock;
    return b > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(b);
}

bool AtapiCdrom::read_block(u32 lba, u8* out)
{
    return image_ && lba < blocks() && image_->read(static_cast<u64>(lba) * kPerBlock, kPerBlock, out);
}

AtapiCdrom::Result AtapiCdrom::check(u8 key, u8 asc, u8 ascq)
{
    sense_key_ = key;
    asc_ = asc;
    ascq_ = ascq;
    error_reg_ = static_cast<u8>((key << 4) | kAbrt);
    Result r;
    r.error = true;
    return r;
}

AtapiCdrom::Result AtapiCdrom::data(u32 len, u32 alloc)
{
    error_reg_ = 0;
    Result r;
    const u32 n = len < alloc ? len : alloc;
    if (n > 0) {
        r.kind = Result::Kind::DataIn;
        r.length = n;
    }
    return r;
}

bool AtapiCdrom::ready(Result& r)
{
    if (!image_) {
        r = check(kNotReady, kAscNoMedium);
        return false;
    }
    if (unit_attention_) {
        unit_attention_ = false;
        r = check(kUnitAttention, kAscMediumChanged);
        return false;
    }
    return true;
}

AtapiCdrom::Result AtapiCdrom::command(const u8* cdb, u8* out)
{
    const Result r = run(cdb, out);
    if (trace_)
        trace_(trace_ctx_, cdb, r.error ? (static_cast<u32>(sense_key_) << 16) | (asc_ << 8) | ascq_ : 0);
    return r;
}

AtapiCdrom::Result AtapiCdrom::run(const u8* cdb, u8* out)
{
    for (u32 i = 0; i < kResponseMax; ++i)
        out[i] = 0;
    const u8 op = cdb[0];
    if (op != 0x03) {  // every command but REQUEST SENSE starts with clean sense data
        sense_key_ = asc_ = ascq_ = 0;
        error_reg_ = 0;
    }
    Result r;
    switch (op) {
    case 0x00:  // TEST UNIT READY
        return ready(r) ? good() : r;

    case 0x03: {  // REQUEST SENSE: fixed format, then the sense data is consumed
        out[0] = 0x70;
        out[2] = sense_key_;
        out[7] = 10;  // additional sense length
        out[12] = asc_;
        out[13] = ascq_;
        sense_key_ = asc_ = ascq_ = 0;
        return data(18, cdb[4]);
    }

    case 0x12:  // INQUIRY
        if (cdb[1] & 1)  // vital product data pages: not supported
            return check(kIllegalRequest, kAscInvalidField);
        out[0] = 0x05;   // CD/DVD device
        out[1] = 0x80;   // removable medium
        out[2] = 0x00;
        out[3] = 0x21;   // ATAPI version 2, response data format 1 (as DingusPPC's drive)
        out[4] = 31;     // additional length
        put_text(out + 8, 8, "G3MacPi");
        put_text(out + 16, 16, "Virtual CD-ROM");
        put_text(out + 32, 4, "0.1");
        return data(36, be16(cdb + 3));

    case 0x1B:  // START STOP UNIT
        if ((cdb[4] & 0x03) == 0x02) {  // LoEj, not Start: eject
            if (locked_)
                return check(kIllegalRequest, kAscRemovalPrevented, 0x02);
            image_ = nullptr;
            unit_attention_ = false;
            if (eject_cb_)
                eject_cb_(eject_ctx_);
        }
        return good();

    case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
        locked_ = (cdb[4] & 1) != 0;
        return good();

    case 0x25: {  // READ CAPACITY
        if (!ready(r))
            return r;
        put32(out, blocks() - 1);
        put32(out + 4, kBlock);
        return data(8, 8);
    }

    case 0x28:    // READ(10)
    case 0xA8: {  // READ(12)
        if (!ready(r))
            return r;
        const u32 lba = be32(cdb + 2);
        const u32 count = op == 0x28 ? be16(cdb + 7) : be32(cdb + 6);
        if (static_cast<u64>(lba) + count > blocks())
            return check(kIllegalRequest, kAscLbaOutOfRange);
        if (count == 0)
            return good();
        r.kind = Result::Kind::SectorsIn;
        r.lba = lba;
        r.count = count;
        return r;
    }

    case 0xBE: {  // READ CD: 2048-byte user data only
        if (!ready(r))
            return r;
        const u32 lba = be32(cdb + 2);
        const u32 count = be24(cdb + 6);
        const u32 type = (cdb[1] >> 2) & 7;  // expected sector type: any or mode 1
        if (count && ((cdb[9] & 0xF8) != 0x10 || (type != 0 && type != 2) || cdb[10]))
            return check(kIllegalRequest, kAscInvalidField);
        if (static_cast<u64>(lba) + count > blocks())
            return check(kIllegalRequest, kAscLbaOutOfRange);
        if (count == 0)
            return good();
        r.kind = Result::Kind::SectorsIn;
        r.lba = lba;
        r.count = count;
        return r;
    }

    case 0x43: {  // READ TOC
        if (!ready(r))
            return r;
        const bool msf = (cdb[1] & 2) != 0;
        u32 format = cdb[2] & 0x0F;
        if (format == 0)
            format = cdb[9] >> 6;  // the older ATAPI/Apple place for the format
        const u32 alloc = be16(cdb + 7);
        if (format == 0) {
            const u32 start = cdb[6];
            u32 n = 0;
            u8* d = out + 4;
            if (start <= 1) {  // track 1 and the lead-out
                d[1] = kTrackAdrCtrl;
                d[2] = 1;
                put_address(d + 4, 0, msf);
                d += 8;
                ++n;
            } else if (start != kLeadOut) {
                return check(kIllegalRequest, kAscInvalidField);
            }
            d[1] = kTrackAdrCtrl;
            d[2] = kLeadOut;
            put_address(d + 4, blocks(), msf);
            ++n;
            put16(out, 2 + 8 * n);  // TOC data length (excludes itself)
            out[2] = 1;             // first track
            out[3] = 1;             // last track
            return data(4 + 8 * n, alloc);
        }
        if (format == 1) {  // session information: one session, track 1
            put16(out, 10);
            out[2] = 1;
            out[3] = 1;
            out[5] = kTrackAdrCtrl;
            out[6] = 1;
            put_address(out + 8, 0, msf);
            return data(12, alloc);
        }
        return check(kIllegalRequest, kAscInvalidField);
    }

    case 0x1A:    // MODE SENSE(6)
    case 0x5A: {  // MODE SENSE(10)
        bool ok = true;
        const bool ten = op == 0x5A;
        const u32 n = mode_sense(cdb, out, ten, ok);
        if (!ok)
            return check(kIllegalRequest, kAscInvalidField);
        return data(n, ten ? be16(cdb + 7) : cdb[4]);
    }

    case 0x55: {  // MODE SELECT(10): parameters accepted and ignored
        Result o;
        o.length = be16(cdb + 7);
        if (o.length)
            o.kind = Result::Kind::DataOut;
        return o;
    }

    case 0xBB:  // SET CD SPEED
        return good();

    case 0x46: {  // GET CONFIGURATION: the CD-ROM profile and the Core feature
        put32(out, 12);                       // data length (excludes itself)
        put16(out + 6, image_ ? 0x0008 : 0);  // current profile: CD-ROM
        put16(out + 8, 0x0001);               // Core feature
        out[10] = 0x03;                       // persistent, current
        out[11] = 4;
        put32(out + 12, 2);                   // physical interface: ATAPI
        return data(16, be16(cdb + 7));
    }

    default:
        return check(kIllegalRequest, kAscInvalidCommand);
    }
}

u32 AtapiCdrom::mode_sense(const u8* cdb, u8* out, bool ten, bool& ok)
{
    const u32 page = cdb[2] & 0x3F;
    const u32 header = ten ? 8 : 4;
    u8* p = out + header;
    auto add = [&](u32 code) {
        if (code == 0x01) {  // read error recovery
            p[0] = 0x01;
            p[1] = 6;
            p += 8;
        } else if (code == 0x0E) {  // CD audio control
            p[0] = 0x0E;
            p[1] = 14;
            p[2] = 0x04;  // Immed
            put16(p + 6, 75);
            p[8] = 0x01;
            p[9] = 0xFF;
            p[10] = 0x02;
            p[11] = 0xFF;
            p += 16;
        } else if (code == 0x2A) {  // CD capabilities and mechanical status
            p[0] = 0x2A;
            p[1] = 0x18;
            p[6] = static_cast<u8>(0x01 | (locked_ ? 0x02 : 0) | 0x08 | (1 << 5));  // lock, eject, tray
            put16(p + 8, 5632);   // maximum read speed, kB/s (32x)
            put16(p + 10, 256);   // volume levels
            put16(p + 12, 128);   // buffer, KB
            put16(p + 14, 5632);  // current read speed
            p += 26;
        } else if (code == 0x31) {  // Apple's page
            p[0] = 0x31;
            p[1] = 6;
            p[2] = '.';
            p[3] = 'A';
            p[4] = 'p';
            p[5] = 'p';
            p += 8;
        }
    };
    if (page == 0x3F) {
        add(0x01);
        add(0x0E);
        add(0x2A);
        add(0x31);
    } else if (page == 0x01 || page == 0x0E || page == 0x2A || page == 0x31) {
        add(page);
    } else {
        ok = false;
        return 0;
    }
    const u32 total = static_cast<u32>(p - out);
    const u8 medium = image_ ? 0x01 : 0x70;  // 120 mm data disc / no disc
    if (ten) {
        put16(out, total - 2);
        out[2] = medium;
    } else {
        out[0] = static_cast<u8>(total - 1);
        out[1] = medium;
    }
    return total;
}

void AtapiCdrom::identify(u16* w)
{
    for (u32 i = 0; i < 256; ++i)
        w[i] = 0;
    w[0] = 0x85C0;                          // ATAPI, CD-ROM, removable, 12-byte packets
    put_words(w, 10, 10, "G3MP00000002");   // serial number
    put_words(w, 23, 4, "0.1");             // firmware revision
    put_words(w, 27, 20, "G3MacPi Virtual CD-ROM");
    w[49] = 0x0300;                         // LBA, DMA supported
    w[53] = 0x0003;                         // words 64-70 valid
    w[63] = 0x0007;                         // multiword DMA modes 0-2
    w[64] = 0x0003;                         // PIO modes 3 and 4
    w[65] = 120;
    w[66] = 120;
    w[67] = 120;
    w[68] = 120;
    w[80] = 0x003E;                         // ATA/ATAPI-1 to -5
}

} // namespace g3
