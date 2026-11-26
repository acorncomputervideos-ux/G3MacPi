// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/storage/new_mac_disk: a new disk from an existing Mac OS
// disk's drivers, read back with the independent readers.

#include "core/common/endian.h"
#include "core/storage/apple_partition_map.h"
#include "core/storage/block_device.h"
#include "core/storage/hfs_volume.h"
#include "core/storage/new_mac_disk.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

namespace {

// A source disk laid out as Drive Setup lays out the user's 1 GB disk
// (scaled down): DDM with two drivers, the map (63 blocks), two
// Apple_Driver_ATA, Apple_Driver_IOKit, Apple_Patches, then Apple_HFS,
// a second volume and Apple_Free. Each driver partition is filled with a
// byte of its own.
struct Source {
    static constexpr u32 kBlocks = 20000, kHfs = 1216;
    std::vector<u8> d = std::vector<u8>(kBlocks * 512u, 0xEE);
    void entry(u32 n, u32 count, u32 start, u32 blocks, const char* name, const char* type, u32 status)
    {
        u8* p = &d[n * 512u];
        std::memset(p, 0, 512);
        store_be16(p, 0x504D);
        store_be32(p + 4, count);
        store_be32(p + 8, start);
        store_be32(p + 12, blocks);
        std::memcpy(p + 16, name, std::strlen(name));
        std::memcpy(p + 48, type, std::strlen(type));
        store_be32(p + 84, blocks);
        store_be32(p + 88, status);
    }
    Source()
    {
        std::memset(d.data(), 0, 64 * 512);
        store_be16(&d[0], 0x4552);
        store_be16(&d[2], 512);
        store_be32(&d[4], kBlocks - 1);  // Drive Setup: the size less one
        store_be16(&d[16], 2);
        store_be32(&d[18], 64);
        store_be16(&d[22], 21);
        store_be16(&d[24], 0x701);
        const u32 n = 8;
        entry(1, n, 1, 63, "Apple", "Apple_partition_map", 0x3);
        entry(2, n, 64, 54, "Macintosh", "Apple_Driver_ATA", 0x77f);
        entry(3, n, 118, 74, "Macintosh", "Apple_Driver_ATA", 0x37f);
        entry(4, n, 192, 512, "Macintosh", "Apple_Driver_IOKit", 0);
        entry(5, n, 704, 512, "Patch Partition", "Apple_Patches", 0);
        entry(6, n, kHfs, 10000, "untitled", "Apple_HFS", 0x4400037f);
        entry(7, n, 11216, 8000, "Second", "Apple_HFS", 0x37f);
        entry(8, n, 19216, 768, "Extra", "Apple_Free", 0);
        for (u32 b = 64; b < kHfs; ++b)
            std::memset(&d[b * 512u], static_cast<int>(b < 118 ? 0xA1 : b < 192 ? 0xA2 : b < 704 ? 0xA3 : 0xA4), 512);
    }
};

} // namespace

TEST_CASE("storage/new_mac_disk/drivers_copied_and_a_new_volume")
{
    Source s;
    RamBlockDevice src(s.d.data(), s.d.size(), true);
    const u64 bytes = 40000ull * 512;
    NewMacDisk nd;
    REQUIRE(nd.plan(src, bytes));
    CHECK_EQ(nd.hfs_start(), Source::kHfs);
    CHECK_EQ(nd.alt_mdb_offset(), bytes - 1024);
    std::vector<u8> disk(bytes, 0x5A);  // the rest of the file: anything
    std::vector<u8> head(nd.head_bytes());
    u8 alt[512];
    HfsFormatOptions opt;
    opt.volume_name = "New Disk";
    opt.date = 0xC0000000u;
    REQUIRE(nd.build(src, opt, head.data(), alt));
    std::memcpy(disk.data(), head.data(), head.size());
    std::memcpy(&disk[nd.alt_mdb_offset()], alt, 512);
    RamBlockDevice dev(disk.data(), disk.size());

    // The driver descriptor map: the new size (less one, as the source's),
    // its driver entries untouched; the drivers' blocks copied.
    CHECK_EQ(load_be32(&disk[4]), 40000u - 1);
    CHECK_EQ(load_be16(&disk[16]), 2u);
    CHECK_EQ(load_be32(&disk[18]), 64u);
    CHECK_EQ(disk[64 * 512u], 0xA1);
    CHECK_EQ(disk[150 * 512u], 0xA2);
    CHECK_EQ(disk[500 * 512u], 0xA3);
    CHECK_EQ(disk[1000 * 512u + 7], 0xA4);

    // The map: the map, the drivers and patches, then one volume to the end.
    ApplePartitionMap apm;
    REQUIRE(apm.read(dev));
    CHECK_EQ(apm.count(), 6u);
    const char* types[6] = {"Apple_partition_map", "Apple_Driver_ATA", "Apple_Driver_ATA", "Apple_Driver_IOKit",
                            "Apple_Patches", "Apple_HFS"};
    for (u32 i = 0; i < 6; ++i) {
        const ApmPartition* p = apm.partition(i + 1);
        REQUIRE(p != nullptr);
        CHECK(std::strcmp(p->type, types[i]) == 0);
        CHECK_EQ(load_be32(&disk[(i + 1) * 512u + 4]), 6u);  // pmMapBlkCnt in every entry
    }
    CHECK_EQ(apm.partition(2)->start, 64u);
    CHECK_EQ(apm.partition(2)->status, 0x77fu);
    const ApmPartition* v = apm.partition(6);
    CHECK_EQ(v->start, Source::kHfs);
    CHECK_EQ(v->blocks, 40000u - Source::kHfs);
    CHECK(std::strcmp(v->name, "New Disk") == 0);
    CHECK_EQ(v->status, 0x4400037fu);
    CHECK_EQ(load_be32(&disk[6 * 512u + 84]), 40000u - Source::kHfs);
    for (u32 b = 7; b < 64; ++b)
        CHECK_EQ(load_be32(&disk[b * 512u]), 0u);  // the rest of the map: empty

    // The volume mounts with our reader, empty, named; its alternate MDB is
    // second-last in the partition.
    BlockWindow win(dev, v->start, v->blocks);
    HfsVolume fs;
    REQUIRE(fs.mount(win));
    CHECK(fs.classic());
    u32 entries = 0;
    REQUIRE(fs.list(HfsVolume::kRootFolder, [](void* c, const HfsEntry&) {
        ++*static_cast<u32*>(c);
        return true;
    }, &entries));
    CHECK_EQ(entries, 0u);
    CHECK_EQ(load_be16(&disk[(Source::kHfs + 2) * 512u]), 0x4244u);
    CHECK(std::memcmp(&disk[(Source::kHfs + 2) * 512u], &disk[bytes - 1024], 512) == 0);
}

TEST_CASE("storage/new_mac_disk/unusable_sources_and_sizes")
{
    NewMacDisk nd;
    {
        std::vector<u8> blank(4096 * 512, 0);  // a blank disk: no map
        RamBlockDevice src(blank.data(), blank.size(), true);
        CHECK(!nd.plan(src, 1u << 30));
        CHECK(std::strstr(nd.why(), "no Apple partition map") != nullptr);
    }
    {
        Source s;  // no ATA driver: our own format_apm_hfs style disk
        for (u32 e : {2u, 3u})
            std::memcpy(&s.d[e * 512u + 48], "Apple_Free\0\0\0\0\0\0\0\0", 18);
        RamBlockDevice src(s.d.data(), s.d.size(), true);
        CHECK(!nd.plan(src, 1u << 30));
        CHECK(std::strstr(nd.why(), "no ATA driver") != nullptr);
    }
    {
        Source s;
        RamBlockDevice src(s.d.data(), s.d.size(), true);
        CHECK(!nd.plan(src, (Source::kHfs + 100) * 512ull));  // no room for a volume
        CHECK(!nd.plan(src, 40000ull * 512 + 3));             // not whole blocks
        CHECK(nd.plan(src, 4095ull << 20));                   // the menu's largest disk
        CHECK(nd.head_bytes() < (16u << 20));                 // only the start is built
    }
}
