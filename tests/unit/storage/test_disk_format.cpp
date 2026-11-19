// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/storage/disk_format: new Apple-partitioned HFS disks.
//
// Expected values are worked out by hand from the layout rules in
// disk_format.h (Inside Macintosh: Files and Devices); the result is then
// read back with the independent readers (ApplePartitionMap, HfsVolume).

#include "core/common/endian.h"
#include "core/storage/apple_partition_map.h"
#include "core/storage/block_device.h"
#include "core/storage/disk_format.h"
#include "core/storage/hfs_volume.h"
#include "tests/framework/g3test.h"

#include <cstring>
#include <vector>

using namespace g3;

namespace {
u16 be16(const u8* p) { return load_be16(p); }
u32 be32(const u8* p) { return load_be32(p); }
} // namespace

TEST_CASE("storage/disk_format/apm_layout")
{
    const u32 sectors = 65536;  // 32 MB
    std::vector<u8> disk(static_cast<size_t>(sectors) * 512, 0xAA);
    HfsFormatOptions opt;
    opt.volume_name = "Test Disk";
    opt.date = 0xB4000000u;
    REQUIRE(format_apm_hfs(disk.data(), disk.size(), opt));

    // Driver descriptor map.
    CHECK_EQ(be16(&disk[0]), 0x4552u);   // 'ER'
    CHECK_EQ(be16(&disk[2]), 512u);
    CHECK_EQ(be32(&disk[4]), sectors);
    CHECK_EQ(be16(&disk[16]), 0u);       // no drivers

    // Read back with the partition-map reader.
    RamBlockDevice dev(disk.data(), disk.size());
    ApplePartitionMap apm;
    REQUIRE(apm.read(dev));
    CHECK_EQ(apm.count(), 2u);
    const ApmPartition* map = apm.partition(1);
    REQUIRE(map != nullptr);
    CHECK(std::strcmp(map->type, "Apple_partition_map") == 0);
    CHECK_EQ(map->start, 1u);
    CHECK_EQ(map->blocks, 63u);
    const u32 n = apm.find_type("Apple_HFS");
    REQUIRE(n == 2u);
    const ApmPartition* hfs = apm.partition(n);
    CHECK_EQ(hfs->start, kApmHfsStart);
    CHECK_EQ(hfs->blocks, sectors - kApmHfsStart);
    CHECK(std::strcmp(hfs->name, "Test Disk") == 0);
    // The map's own blocks between the entries and the volume are cleared.
    CHECK_EQ(disk[3 * 512], 0u);
    CHECK_EQ(disk[63 * 512 + 511], 0u);
}

TEST_CASE("storage/disk_format/hfs_mdb_by_hand")
{
    // A 32 MB disk: the HFS partition is 65536 - 64 = 65472 sectors.
    //  usable = 65472 - 5 = 65467 sectors -> 512-byte allocation blocks fit
    //  (65467 <= 65535). Bitmap: ceil(65467 / 4096) = 16 sectors, so
    //  drAlBlSt = 3 + 16 = 19, drNmAlBlks = 65472 - 19 - 2 = 65451.
    //  B-tree files: 32 MB / 256 = 128 KB each = 256 allocation blocks.
    std::vector<u8> disk(65536u * 512u);
    HfsFormatOptions opt;
    opt.volume_name = "Test Disk";
    opt.date = 0xB4000000u;
    REQUIRE(format_apm_hfs(disk.data(), disk.size(), opt));
    const u8* vol = &disk[kApmHfsStart * 512];
    const u8* mdb = vol + 1024;
    CHECK_EQ(be16(mdb + 0), 0x4244u);
    CHECK_EQ(be32(mdb + 2), 0xB4000000u);
    CHECK_EQ(be16(mdb + 10), 0x0100u);       // cleanly unmounted
    CHECK_EQ(be16(mdb + 14), 3u);
    CHECK_EQ(be16(mdb + 18), 65451u);
    CHECK_EQ(be32(mdb + 20), 512u);
    CHECK_EQ(be16(mdb + 28), 19u);
    CHECK_EQ(be32(mdb + 30), 16u);           // next catalog node ID
    CHECK_EQ(be16(mdb + 34), 65451u - 512u); // free blocks
    CHECK_EQ(mdb[36], 9u);
    CHECK(std::memcmp(mdb + 37, "Test Disk", 9) == 0);
    CHECK_EQ(be32(mdb + 130), 131072u);      // extents file size
    CHECK_EQ(be16(mdb + 134), 0u);
    CHECK_EQ(be16(mdb + 136), 256u);
    CHECK_EQ(be32(mdb + 146), 131072u);      // catalog file size
    CHECK_EQ(be16(mdb + 150), 256u);
    CHECK_EQ(be16(mdb + 152), 256u);
    // Alternate MDB in the second-last sector of the volume.
    CHECK(std::memcmp(vol + (65472u - 2u) * 512u, mdb, 512) == 0);
    // Catalog leaf (node 1 of the catalog at allocation block 256):
    //   record 0 at 14: key {len 15, 0, parent 1, "Test Disk"} = 16 bytes
    //     (odd, padded), folder record: type 1, dirID 2;
    //   record 1 at 14 + 16 + 70 = 100: key {6, 0, parent 2, ""} = 8 bytes,
    //     thread: type 3, thdParID 1 @10, name "Test Disk" @14.
    const u8* leaf = vol + 19u * 512u + 256u * 512u + 512u;
    CHECK_EQ(leaf[8], 0xFFu);                 // leaf node
    CHECK_EQ(be16(leaf + 10), 2u);            // two records
    CHECK_EQ(be16(leaf + 510), 14u);
    CHECK_EQ(be16(leaf + 508), 100u);
    CHECK_EQ(be16(leaf + 506), 154u);         // free space: 100 + 8 + 46
    CHECK_EQ(leaf[14], 15u);
    CHECK_EQ(be32(leaf + 16), 1u);
    CHECK_EQ(leaf[30], 1u);                   // folder
    CHECK_EQ(be32(leaf + 36), 2u);            // dirID
    CHECK_EQ(leaf[100], 6u);
    CHECK_EQ(be32(leaf + 102), 2u);
    CHECK_EQ(leaf[108], 3u);                  // thread
    CHECK_EQ(be32(leaf + 118), 1u);           // parent of the root
    CHECK_EQ(leaf[122], 9u);
    CHECK(std::memcmp(leaf + 123, "Test Disk", 9) == 0);
    // Bitmap: exactly the first 512 allocation blocks are in use.
    const u8* bitmap = vol + 3 * 512;
    for (u32 i = 0; i < 64; ++i)
        CHECK_EQ(bitmap[i], 0xFFu);
    CHECK_EQ(bitmap[64], 0u);
}

TEST_CASE("storage/disk_format/mounts_with_our_reader")
{
    std::vector<u8> disk(65536u * 512u);
    HfsFormatOptions opt;
    opt.volume_name = "Macintosh HD";
    REQUIRE(format_apm_hfs(disk.data(), disk.size(), opt));
    RamBlockDevice dev(disk.data(), disk.size());
    ApplePartitionMap apm;
    REQUIRE(apm.read(dev));
    const ApmPartition* p = apm.partition(apm.find_type("Apple_HFS"));
    REQUIRE(p != nullptr);
    BlockWindow win(dev, p->start, p->blocks);
    HfsVolume fs;
    REQUIRE(fs.mount(win));
    CHECK(fs.classic());
    CHECK_EQ(fs.block_size(), 512u);
    // The root folder is empty; its thread names the volume.
    u32 entries = 0;
    REQUIRE(fs.list(HfsVolume::kRootFolder, [](void* c, const HfsEntry&) {
        ++*static_cast<u32*>(c);
        return true;
    }, &entries));
    CHECK_EQ(entries, 0u);
    HfsEntry e;
    CHECK(!fs.lookup(HfsVolume::kRootFolder, "System Folder", e));
}

TEST_CASE("storage/disk_format/larger_disks_and_rejects")
{
    // 200 MB: 409600 - 64 sectors no longer fit 16-bit 512-byte blocks;
    // the allocation block grows to 3584 bytes (7 sectors): 409531 * 512 /
    // 3584 = 58504 blocks.
    std::vector<u8> disk(409600u * 512u);
    HfsFormatOptions opt;
    REQUIRE(format_apm_hfs(disk.data(), disk.size(), opt));
    const u8* mdb = &disk[kApmHfsStart * 512 + 1024];
    CHECK_EQ(be32(mdb + 20), 3584u);
    CHECK(be16(mdb + 18) <= 0xFFFFu);
    RamBlockDevice dev(disk.data(), disk.size());
    BlockWindow win(dev, kApmHfsStart, 409600u - kApmHfsStart);
    HfsVolume fs;
    CHECK(fs.mount(win));

    std::vector<u8> small(100u * 1024u);
    CHECK(!format_hfs(small.data(), small.size(), opt));     // below 800 KB
    std::vector<u8> ok(1024u * 1024u);
    HfsFormatOptions bad;
    bad.volume_name = "a:b";
    CHECK(!format_hfs(ok.data(), ok.size(), bad));           // ':' not allowed
    bad.volume_name = "";
    CHECK(!format_hfs(ok.data(), ok.size(), bad));
    bad.volume_name = "This name is far too long for HFS";
    CHECK(!format_hfs(ok.data(), ok.size(), bad));
    CHECK(!format_hfs(ok.data(), ok.size() - 1, opt));       // not whole sectors
}
