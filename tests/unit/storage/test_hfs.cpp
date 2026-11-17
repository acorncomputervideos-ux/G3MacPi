// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the HFS Plus reader. The images are built here from the
// TN1150 layout (the builder is written separately from the reader, but
// by the same author: see docs/storage/hfs.md on independent images).

#include "core/storage/hfs_volume.h"
#include "tests/framework/g3test.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace g3;

#include "tests/unit/storage/hfs_image.h"

using namespace hfs_image;

namespace {

std::string utf8(const HfsEntry& e)
{
    std::string s;
    for (u32 i = 0; i < e.name_len; ++i)
        s += static_cast<char>(e.name[i]);
    return s;
}

} // namespace

TEST_CASE("storage/hfsplus/mount_and_lookup")
{
    Volume vol;
    RamBlockDevice dev(vol.img.d.data(), vol.img.d.size());
    HfsVolume fs;
    REQUIRE(fs.mount(dev));
    CHECK(!fs.wrapped());
    CHECK_EQ(fs.block_size(), kBs);
    CHECK_EQ(fs.blessed_folder(), 17u);
    HfsEntry e;
    REQUIRE(fs.lookup(HfsVolume::kRootFolder, "System Folder", e));
    CHECK(e.folder);
    CHECK_EQ(e.id, 17u);
    CHECK(fs.lookup(2, "system folder", e));                // ASCII case-insensitive
    CHECK(!fs.lookup(2, "System", e));
    CHECK(!fs.lookup(2, "System Folder2", e));
    REQUIRE(fs.lookup(17, "Mac OS ROM", e));                // in the third leaf
    CHECK(!e.folder);
    CHECK_EQ(e.type, fourcc("tbxi"));
    CHECK_EQ(e.creator, fourcc("chrp"));
    CHECK_EQ(e.data.logical_size, Volume::kRomSize);
    REQUIRE(fs.lookup(17, "Finder", e));
    CHECK_EQ(e.id, 31u);
    CHECK(!fs.lookup(17, "ReadMe", e));                     // that one is in the root
    CHECK(!fs.lookup(99, "Finder", e));
    // By Finder type, as Open Firmware's "\\:tbxi" does.
    REQUIRE(fs.find_by_type(fs.blessed_folder(), fourcc("tbxi"), e));
    CHECK(utf8(e) == "Mac OS ROM");
    CHECK(!fs.find_by_type(2, fourcc("tbxi"), e));
    // Listing a folder spanning two leaf nodes.
    std::vector<std::string> names;
    fs.list(17, [](void* c, const HfsEntry& x) {
        static_cast<std::vector<std::string>*>(c)->push_back(utf8(x));
        return true;
    }, &names);
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Finder");
    CHECK(names[2] == "Mac OS ROM");
}

TEST_CASE("storage/hfsplus/read_data_and_overflow_extents")
{
    Volume vol;
    RamBlockDevice dev(vol.img.d.data(), vol.img.d.size());
    HfsVolume fs;
    REQUIRE(fs.mount(dev));
    HfsEntry e;
    REQUIRE(fs.lookup(17, "Mac OS ROM", e));
    std::vector<u8> buf(20000, 0xEE);
    CHECK_EQ(fs.read(e.data, 0, buf.data(), 20000), static_cast<s64>(Volume::kRomSize));
    CHECK(std::memcmp(buf.data(), vol.rom.data(), vol.rom.size()) == 0);
    CHECK_EQ(buf[Volume::kRomSize], 0xEEu);                     // nothing past logicalSize
    CHECK_EQ(fs.read(e.data, 4095, buf.data(), 3), 3);         // across a block boundary
    CHECK(std::memcmp(buf.data(), &vol.rom[4095], 3) == 0);
    CHECK_EQ(fs.read(e.data, Volume::kRomSize, buf.data(), 10), 0);

    REQUIRE(fs.lookup(17, "Fragmented", e));
    std::vector<u8> all(Volume::kFragSize);
    CHECK_EQ(fs.read(e.data, 0, all.data(), static_cast<u32>(all.size())),
             static_cast<s64>(Volume::kFragSize));
    CHECK(all == vol.frag);                                     // includes blocks from the overflow file
    CHECK_EQ(fs.read(e.data, 9 * kBs + 7, buf.data(), 16), 16);  // only in the overflow record
    CHECK(std::memcmp(buf.data(), &vol.frag[9 * kBs + 7], 16) == 0);
}

TEST_CASE("storage/hfsplus/wrapper_and_rejects")
{
    Volume vol;
    // HFS wrapper: MDB at 1024, embedded volume at drAlBlSt*512 + start*drAlBlkSiz.
    const u32 al_blk = 2048, al_st = 16, embed_start = 3;
    const size_t offset = al_st * 512 + embed_start * al_blk;   // 14336
    Img w(offset + vol.img.d.size());
    w.u16_(1024 + 0, 0x4244);                // 'BD'
    w.u32_(1024 + 20, al_blk);               // drAlBlkSiz
    w.u16_(1024 + 28, al_st);                // drAlBlSt
    w.u16_(1024 + 0x7C, 0x482B);             // drEmbedSigWord
    w.u16_(1024 + 0x7E, embed_start);        // drEmbedExtent.startBlock
    w.u16_(1024 + 0x80, static_cast<u16>(vol.img.d.size() / al_blk));
    std::memcpy(&w.d[offset], vol.img.d.data(), vol.img.d.size());
    RamBlockDevice dev(w.d.data(), w.d.size());
    HfsVolume fs;
    REQUIRE(fs.mount(dev));
    CHECK(fs.wrapped());
    CHECK_EQ(fs.volume_offset(), offset);
    HfsEntry e;
    REQUIRE(fs.find_by_type(fs.blessed_folder(), fourcc("tbxi"), e));
    std::vector<u8> buf(16);
    CHECK_EQ(fs.read(e.data, 100, buf.data(), 16), 16);
    CHECK(std::memcmp(buf.data(), &vol.rom[100], 16) == 0);

    // Plain HFS without an embedded volume, bad signature, bad block size.
    Img hfs(8192);
    hfs.u16_(1024, 0x4244);
    RamBlockDevice hd(hfs.d.data(), hfs.d.size());
    CHECK(!fs.mount(hd));
    Volume bad;
    bad.img.u16_(1024, 0x1234);
    RamBlockDevice bd(bad.img.d.data(), bad.img.d.size());
    CHECK(!fs.mount(bd));
    Volume bad2;
    bad2.img.u32_(1024 + 40, 3000);
    RamBlockDevice bd2(bad2.img.d.data(), bad2.img.d.size());
    CHECK(!fs.mount(bd2));
    CHECK(!fs.lookup(2, "System Folder", e));                 // not mounted
}

// --- classic HFS ---------------------------------------------------------------------

TEST_CASE("storage/hfs_classic/mount_lookup_and_read")
{
    ClassicVolume vol;
    RamBlockDevice dev(vol.img.d.data(), vol.img.d.size());
    HfsVolume fs;
    REQUIRE(fs.mount(dev));
    CHECK(fs.classic());
    CHECK(!fs.wrapped());
    CHECK_EQ(fs.block_size(), ClassicVolume::kAlBlk);
    CHECK_EQ(fs.blessed_folder(), 17u);
    HfsEntry e;
    REQUIRE(fs.lookup(HfsVolume::kRootFolder, "SYSTEM FOLDER", e));
    CHECK(e.folder);
    CHECK_EQ(e.id, 17u);
    REQUIRE(fs.find_by_type(fs.blessed_folder(), fourcc("tbxi"), e));   // in the third leaf
    CHECK(utf8(e) == "Mac OS ROM");
    CHECK_EQ(e.creator, fourcc("chrp"));
    CHECK_EQ(e.id, 32u);
    std::vector<u8> buf(8000, 0xEE);
    CHECK_EQ(fs.read(e.data, 0, buf.data(), 8000), static_cast<s64>(ClassicVolume::kRomSize));
    CHECK(std::memcmp(buf.data(), vol.rom.data(), vol.rom.size()) == 0);
    CHECK_EQ(fs.read(e.data, 1535, buf.data(), 2), 2);                 // across an allocation block
    CHECK(std::memcmp(buf.data(), &vol.rom[1535], 2) == 0);
    REQUIRE(fs.lookup(17, "Finder", e));
    CHECK_EQ(e.type, fourcc("FNDR"));
    CHECK(!fs.lookup(17, "Nothing", e));
    // A file whose last two extents are in the extents overflow file.
    REQUIRE(fs.lookup(17, "Fragmented", e));
    std::vector<u8> all(ClassicVolume::kFragSize);
    CHECK_EQ(fs.read(e.data, 0, all.data(), static_cast<u32>(all.size())),
             static_cast<s64>(ClassicVolume::kFragSize));
    CHECK(all == vol.frag);
    std::vector<std::string> names;
    fs.list(17, [](void* c, const HfsEntry& x) {
        static_cast<std::vector<std::string>*>(c)->push_back(utf8(x));
        return true;
    }, &names);
    REQUIRE(names.size() == 3);
    CHECK(names[1] == "Fragmented");
}

TEST_CASE("storage/hfs_classic/rejects")
{
    ClassicVolume bad;
    bad.img.u32_(1024 + 20, 1000);              // allocation block not a multiple of 512
    RamBlockDevice d1(bad.img.d.data(), bad.img.d.size());
    HfsVolume fs;
    CHECK(!fs.mount(d1));
    ClassicVolume bad2;
    bad2.img.u32_(1024 + 146, 0);               // no catalog
    RamBlockDevice d2(bad2.img.d.data(), bad2.img.d.size());
    CHECK(!fs.mount(d2));
}
