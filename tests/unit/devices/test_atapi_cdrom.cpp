// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the ATAPI CD-ROM (core/devices/ata/atapi_cdrom + the PACKET
// protocol in ata_bus), driven the way a host driver does (ATA/ATAPI-5).

#include "core/devices/ata/ata_bus.h"
#include "core/devices/ata/atapi_cdrom.h"
#include "core/storage/block_device.h"
#include "tests/framework/g3test.h"

#include <string>
#include <vector>

using namespace g3;
using namespace g3::ata;

namespace {

constexpr u32 kBlocks = 40;

struct Disc {
    std::vector<u8> bytes;
    RamBlockDevice dev;
    Disc() : bytes(kBlocks * 2048), dev(bytes.data(), bytes.size(), true)
    {
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<u8>((i / 2048) * 7 + i % 253);
    }
    u8 at(u32 lba, u32 off) const { return bytes[lba * 2048 + off]; }
};

struct Rig {
    AtaBus bus;
    AtapiCdrom cd;
    Disc disc;
    int edges = 0;
    int ejects = 0;
    Rig()
    {
        bus.set_irq([](void* c, bool a) { if (a) ++*static_cast<int*>(c); }, &edges);
        cd.set_eject_callback([](void* c) { ++*static_cast<int*>(c); }, &ejects);
        bus.attach_cdrom(0, &cd);
    }
    struct Reply {
        std::vector<u8> data;
        u8 status = 0, error = 0, reason = 0;
        u32 blocks = 0;   // DRQ data blocks
    };
    // PACKET with a PIO byte-count limit; reads every data block offered.
    Reply packet(std::vector<u8> cdb, u32 limit = 0xFFFE, u32 dev = 0)
    {
        cdb.resize(12, 0);
        bus.write_reg(kFeatures, 0);
        bus.write_reg(kLbaMid, static_cast<u8>(limit));
        bus.write_reg(kLbaHigh, static_cast<u8>(limit >> 8));
        bus.write_reg(kDevice, static_cast<u8>(dev ? DEV_SLAVE : 0));
        bus.write_reg(kCommand, 0xA0);
        Reply r;
        u8 st = bus.read_alt_status();
        CHECK((st & DRQ) != 0);
        CHECK_EQ(bus.read_reg(kSectorCount), 1u);   // CoD: send the packet
        for (u32 i = 0; i < 12; i += 2)
            bus.write_data(&cdb[i], 2);
        for (int guard = 0; guard < 1000; ++guard) {
            st = bus.read_reg(kStatus);
            const u8 reason = bus.read_reg(kSectorCount);
            if (!(st & DRQ)) {
                r.status = st;
                r.reason = reason;
                r.error = bus.read_reg(kError);
                return r;
            }
            CHECK_EQ(reason, 2u);                    // IO: data to the host
            const u32 n = bus.read_reg(kLbaMid) | (bus.read_reg(kLbaHigh) << 8);
            CHECK(n <= (limit & ~1u));
            std::vector<u8> chunk(n);
            for (u32 i = 0; i < n; i += 2)
                bus.read_data(&chunk[i], 2);
            r.data.insert(r.data.end(), chunk.begin(), chunk.end());
            ++r.blocks;
        }
        CHECK(false);  // never reached the status phase
        return r;
    }
    u8 sense_key()
    {
        const Reply s = packet({0x03, 0, 0, 0, 18});
        return s.data.size() >= 3 ? s.data[2] & 0x0F : 0xFF;
    }
};

std::vector<u8> read10(u32 lba, u32 count)
{
    return {0x28, 0, static_cast<u8>(lba >> 24), static_cast<u8>(lba >> 16), static_cast<u8>(lba >> 8),
            static_cast<u8>(lba), 0, static_cast<u8>(count >> 8), static_cast<u8>(count)};
}

} // namespace

TEST_CASE("devices/atapi/signature_and_identify")
{
    Rig r;
    CHECK_EQ(r.bus.read_reg(kSectorCount), 1u);
    CHECK_EQ(r.bus.read_reg(kLbaLow), 1u);
    CHECK_EQ(r.bus.read_reg(kLbaMid), 0x14u);
    CHECK_EQ(r.bus.read_reg(kLbaHigh), 0xEBu);
    CHECK_EQ(r.bus.read_alt_status(), 0u);           // DRDY clear after reset
    // IDENTIFY DEVICE is aborted, leaving the signature for the host.
    r.bus.write_reg(kLbaMid, 0);
    r.bus.write_reg(kCommand, 0xEC);
    CHECK((r.bus.read_reg(kStatus) & ERR) != 0);
    CHECK_EQ(r.bus.read_reg(kError), ABRT);
    CHECK_EQ(r.bus.read_reg(kLbaMid), 0x14u);
    CHECK_EQ(r.bus.read_reg(kLbaHigh), 0xEBu);
    // IDENTIFY PACKET DEVICE.
    r.bus.write_reg(kCommand, 0xA1);
    CHECK((r.bus.read_reg(kStatus) & DRQ) != 0);
    u8 id[512];
    for (u32 i = 0; i < 512; i += 2)
        r.bus.read_data(&id[i], 2);
    CHECK_EQ(id[0] | (id[1] << 8), 0x85C0u);         // ATAPI, CD-ROM, removable, 12-byte packets
    // Model, word 27: the first character is the word's high byte, which
    // comes second in the (little-endian) data stream.
    CHECK_EQ(id[54], '3');
    CHECK_EQ(id[55], 'G');
    CHECK_EQ(r.bus.read_alt_status() & DRQ, 0u);
}

TEST_CASE("devices/atapi/disk_master_cdrom_slave_signatures")
{
    AtaBus bus;
    std::vector<u8> hd(64 * 512);
    RamBlockDevice disk(hd.data(), hd.size());
    AtapiCdrom cd;
    bus.attach(0, &disk);
    bus.attach_cdrom(1, &cd);
    CHECK_EQ(bus.read_reg(kLbaMid), 0u);             // device 0: an ATA disk
    bus.write_reg(kDevice, DEV_SLAVE);
    CHECK_EQ(bus.read_reg(kLbaMid), 0x14u);          // device 1: ATAPI
    CHECK_EQ(bus.read_reg(kLbaHigh), 0xEBu);
    bus.write_reg(kDevice, 0);
    CHECK_EQ(bus.read_reg(kLbaHigh), 0u);
    bus.write_reg(kLbaMid, 0x55);                    // the host writes the taskfile:
    bus.write_reg(kDevice, DEV_SLAVE);               // no more signatures
    CHECK_EQ(bus.read_reg(kLbaMid), 0x55u);
}

TEST_CASE("devices/atapi/no_disc_then_insert")
{
    Rig r;
    Rig::Reply t = r.packet({0x00});                 // TEST UNIT READY
    CHECK((t.status & ERR) != 0);
    CHECK_EQ(t.error, (0x2 << 4) | ABRT);            // NOT READY
    CHECK_EQ(t.reason, 3u);                          // status phase: CoD | IO
    Rig::Reply s = r.packet({0x03, 0, 0, 0, 18});    // REQUEST SENSE
    REQUIRE(s.data.size() == 18u);
    CHECK_EQ(s.data[0], 0x70u);
    CHECK_EQ(s.data[2], 0x02u);
    CHECK_EQ(s.data[12], 0x3Au);                     // medium not present
    r.cd.insert(&r.disc.dev);
    t = r.packet({0x00});
    CHECK_EQ(t.error >> 4, 0x6u);                    // UNIT ATTENTION once...
    CHECK_EQ(r.packet({0x03, 0, 0, 0, 18}).data[12], 0x28u);  // medium may have changed
    t = r.packet({0x00});
    CHECK_EQ(t.status & ERR, 0u);                    // ...then ready
    CHECK_EQ(t.error, 0u);
}

TEST_CASE("devices/atapi/inquiry_capacity_and_read")
{
    Rig r;
    r.cd.insert(&r.disc.dev);
    r.packet({0x00});                                // consume the unit attention
    Rig::Reply q = r.packet({0x12, 0, 0, 0, 36});
    REQUIRE(q.data.size() == 36u);
    CHECK_EQ(q.data[0], 0x05u);
    CHECK_EQ(q.data[1], 0x80u);
    CHECK(std::string(q.data.begin() + 8, q.data.begin() + 15) == "G3MacPi");
    Rig::Reply c = r.packet({0x25});
    REQUIRE(c.data.size() == 8u);
    CHECK_EQ(c.data[3], kBlocks - 1);                // last LBA
    CHECK_EQ((c.data[6] << 8) | c.data[7], 2048u);
    // READ(10), 3 blocks with a 2048-byte limit: one DRQ block each.
    const int edges0 = r.edges;
    Rig::Reply d = r.packet(read10(5, 3), 2048);
    CHECK_EQ(d.status & ERR, 0u);
    CHECK_EQ(d.blocks, 3u);
    REQUIRE(d.data.size() == 3u * 2048);
    CHECK_EQ(d.data[0], r.disc.at(5, 0));
    CHECK_EQ(d.data[2047], r.disc.at(5, 2047));
    CHECK_EQ(d.data[2 * 2048 + 1000], r.disc.at(7, 1000));
    CHECK_EQ(r.edges - edges0, 4);                   // 3 data blocks + status
    // An odd limit is rounded down; the data still arrives in order.
    Rig::Reply o = r.packet({0xA8, 0, 0, 0, 0, 9, 0, 0, 0, 1}, 1001);   // READ(12) LBA 9
    REQUIRE(o.data.size() == 2048u);
    CHECK_EQ(o.blocks, 3u);                          // 1000 + 1000 + 48
    CHECK_EQ(o.data[1500], r.disc.at(9, 1500));
    // Beyond the disc.
    Rig::Reply e = r.packet(read10(kBlocks - 1, 2));
    CHECK_EQ(e.error >> 4, 0x5u);
    CHECK_EQ(r.packet({0x03, 0, 0, 0, 18}).data[12], 0x21u);
}

TEST_CASE("devices/atapi/read_toc_formats")
{
    Rig r;
    r.cd.insert(&r.disc.dev);
    r.packet({0x00});
    // Format 0, LBA: track 1 at 0, lead-out at kBlocks.
    Rig::Reply t = r.packet({0x43, 0, 0, 0, 0, 0, 0, 0, 100});
    REQUIRE(t.data.size() == 20u);
    CHECK_EQ((t.data[0] << 8) | t.data[1], 18u);
    CHECK_EQ(t.data[2], 1u);
    CHECK_EQ(t.data[3], 1u);
    CHECK_EQ(t.data[5], 0x14u);                      // data track
    CHECK_EQ(t.data[6], 1u);
    CHECK_EQ(t.data[14], 0xAAu);                     // lead-out
    CHECK_EQ(t.data[19], kBlocks);
    // MSF: LBA 0 = 00:02:00; lead-out 40 = 00:02:40 (frames 190 = 2 s 40 f).
    Rig::Reply m = r.packet({0x43, 0x02, 0, 0, 0, 0, 0, 0, 100});
    CHECK_EQ(m.data[9], 0u);
    CHECK_EQ(m.data[10], 2u);
    CHECK_EQ(m.data[11], 0u);
    CHECK_EQ(m.data[18], 2u);
    CHECK_EQ(m.data[19], 40u);
    // Session info, with the format in byte 9 (as Apple's driver sends it).
    Rig::Reply s = r.packet({0x43, 0, 0, 0, 0, 0, 0, 0, 12, 0x40});
    REQUIRE(s.data.size() == 12u);
    CHECK_EQ(s.data[1], 10u);
    CHECK_EQ(s.data[6], 1u);
    // Allocation length cuts the reply.
    CHECK_EQ(r.packet({0x43, 0, 0, 0, 0, 0, 0, 0, 4}).data.size(), 4u);
}

TEST_CASE("devices/atapi/mode_sense_pages")
{
    Rig r;
    r.cd.insert(&r.disc.dev);
    Rig::Reply c = r.packet({0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 255});
    REQUIRE(c.data.size() == 8u + 26u);
    CHECK_EQ((c.data[0] << 8) | c.data[1], 32u);     // mode data length
    CHECK_EQ(c.data[2], 0x01u);                      // 120 mm data disc
    CHECK_EQ(c.data[8], 0x2Au);
    CHECK_EQ(c.data[9], 0x18u);
    CHECK_EQ(c.data[8 + 6] & 0x09, 0x09u);           // lock and eject supported
    Rig::Reply a = r.packet({0x5A, 0, 0x31, 0, 0, 0, 0, 0, 255});
    REQUIRE(a.data.size() == 16u);
    CHECK(std::string(a.data.begin() + 10, a.data.begin() + 14) == ".App");
    Rig::Reply all = r.packet({0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 255});
    CHECK_EQ(all.data.size(), 8u + 8 + 16 + 26 + 8);
    Rig::Reply bad = r.packet({0x5A, 0, 0x05, 0, 0, 0, 0, 0, 255});
    CHECK_EQ(bad.error >> 4, 0x5u);
    CHECK_EQ(r.packet({0x03, 0, 0, 0, 18}).data[12], 0x24u);
    Rig::Reply six = r.packet({0x1A, 0, 0x31, 0, 255});
    REQUIRE(six.data.size() == 12u);
    CHECK_EQ(six.data[0], 11u);
}

TEST_CASE("devices/atapi/eject_lock_and_unknown")
{
    Rig r;
    r.cd.insert(&r.disc.dev);
    r.packet({0x1E, 0, 0, 0, 1});                    // PREVENT removal
    CHECK(r.cd.locked());
    Rig::Reply e = r.packet({0x1B, 0, 0, 0, 0x02});  // eject: refused
    CHECK_EQ(e.error >> 4, 0x5u);
    CHECK(r.cd.image() != nullptr);
    CHECK_EQ(r.ejects, 0);
    r.packet({0x1E, 0, 0, 0, 0});                    // ALLOW
    e = r.packet({0x1B, 0, 0, 0, 0x02});
    CHECK_EQ(e.error, 0u);
    CHECK(r.cd.image() == nullptr);
    CHECK_EQ(r.ejects, 1);
    CHECK_EQ(r.packet({0x00}).error >> 4, 0x2u);     // now: no disc
    Rig::Reply u = r.packet({0xFF});
    CHECK_EQ(u.error >> 4, 0x5u);
    CHECK_EQ(r.packet({0x03, 0, 0, 0, 18}).data[12], 0x20u);  // invalid command
}

TEST_CASE("devices/atapi/dma_read_and_mode_select")
{
    Rig r;
    r.cd.insert(&r.disc.dev);
    r.packet({0x00});
    int dma_requests = 0;
    r.bus.set_dma_request([](void* c) { ++*static_cast<int*>(c); }, &dma_requests);
    std::vector<u8> cdb = read10(2, 2);
    cdb.resize(12, 0);
    r.bus.write_reg(kFeatures, 0x01);                // DMA
    r.bus.write_reg(kCommand, 0xA0);
    for (u32 i = 0; i < 12; i += 2)
        r.bus.write_data(&cdb[i], 2);
    CHECK_EQ(dma_requests, 1);
    CHECK(r.bus.dma_pending());
    const int edges0 = r.edges;
    std::vector<u8> got(4096);
    CHECK_EQ(r.bus.dma_read(got.data(), 3000), 3000u);
    CHECK_EQ(r.edges, edges0);                       // no interrupt mid-transfer
    CHECK_EQ(r.bus.dma_read(got.data() + 3000, 2000), 1096u);  // only what is left
    CHECK_EQ(got[0], r.disc.at(2, 0));
    CHECK_EQ(got[4095], r.disc.at(3, 2047));
    CHECK_EQ(r.edges, edges0 + 1);                   // interrupt at the end
    CHECK_EQ(r.bus.read_reg(kSectorCount), 3u);
    CHECK_EQ(r.bus.read_reg(kStatus) & (DRQ | ERR), 0u);

    // MODE SELECT(10): 8 bytes of parameters, accepted.
    r.bus.write_reg(kFeatures, 0);
    r.bus.write_reg(kCommand, 0xA0);
    const u8 ms[12] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, 8};
    for (u32 i = 0; i < 12; i += 2)
        r.bus.write_data(&ms[i], 2);
    CHECK_EQ(r.bus.read_reg(kSectorCount), 0u);      // data to the device
    CHECK_EQ(r.bus.read_reg(kLbaMid), 8u);
    const u8 params[8] = {};
    r.bus.write_data(params, 8);
    CHECK_EQ(r.bus.read_reg(kSectorCount), 3u);
    CHECK_EQ(r.bus.read_reg(kStatus) & (DRQ | ERR), 0u);
}

TEST_CASE("devices/atapi/read_cd_user_data")
{
    Rig r;
    r.cd.insert(&r.disc.dev);
    r.packet({0x00});
    Rig::Reply d = r.packet({0xBE, 0, 0, 0, 0, 4, 0, 0, 1, 0x10});
    REQUIRE(d.data.size() == 2048u);
    CHECK_EQ(d.data[77], r.disc.at(4, 77));
    Rig::Reply raw = r.packet({0xBE, 0, 0, 0, 0, 4, 0, 0, 1, 0xF8});   // with sync/headers: refused
    CHECK_EQ(raw.error >> 4, 0x5u);
}

TEST_CASE("devices/atapi/packet_trace")
{
    Rig r;
    std::vector<std::pair<u8, u32>> log;
    r.cd.set_packet_trace([](void* c, const u8* cdb, u32 sense) {
        static_cast<std::vector<std::pair<u8, u32>>*>(c)->push_back({cdb[0], sense});
    }, &log);
    r.packet({0x00});                                // no disc: 2/3A/00
    r.packet({0x12, 0, 0, 0, 36});
    REQUIRE(log.size() == 2u);
    CHECK_EQ(log[0].first, 0x00u);
    CHECK_EQ(log[0].second, 0x023A00u);
    CHECK_EQ(log[1].first, 0x12u);
    CHECK_EQ(log[1].second, 0u);
}
