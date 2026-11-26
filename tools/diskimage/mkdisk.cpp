// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/diskimage/mkdisk.cpp — g3_mkdisk: writes a new, empty Mac disk image
// (Apple partition map + one HFS volume) for the emulated ATA disk.
//
//   g3_mkdisk <image> <size in MB> [volume name] [--drivers-from <Mac OS disk image>]
//
// Plain, the volume holds no drivers and no System Folder: Mac OS 9 does
// not mount it (docs/storage/disk-format.md). With --drivers-from, the
// partition map, Apple's ATA drivers and patches are copied from an
// existing Mac OS disk (one Drive Setup initialised) and Mac OS mounts the
// new disk (core/storage/new_mac_disk.h), as the Pi's "New blank disk" does.

#include "core/storage/disk_format.h"
#include "core/storage/new_mac_disk.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

// The source image, read through stdio.
struct FileDevice final : g3::BlockDevice {
    FILE* f = nullptr;
    g3::u64 sectors = 0;
    g3::u64 sector_count() const override { return sectors; }
    bool read_only() const override { return true; }
    bool read(g3::u64 lba, g3::u32 count, g3::u8* out) override
    {
        if (lba + count > sectors || std::fseek(f, static_cast<long>(lba * kSectorSize), SEEK_SET) != 0)
            return false;
        return std::fread(out, kSectorSize, count, f) == count;
    }
    bool write(g3::u64, g3::u32, const g3::u8*) override { return false; }
};

} // namespace

int main(int argc, char** argv)
{
    const char* drivers_from = nullptr;
    std::vector<char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--drivers-from") == 0 && i + 1 < argc)
            drivers_from = argv[++i];
        else
            args.push_back(argv[i]);
    }
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: g3_mkdisk <image> <size in MB> [volume name] [--drivers-from <Mac OS disk>]\n");
        return 2;
    }
    const unsigned long mb = std::strtoul(args[1], nullptr, 10);
    if (mb < 1 || mb > 4095) {
        std::fprintf(stderr, "size must be 1-4095 MB\n");
        return 2;
    }
    g3::HfsFormatOptions opt;
    opt.volume_name = args.size() > 2 ? args[2] : "Untitled";
    // Mac OS dates count seconds from 1904-01-01; Unix time from 1970.
    opt.date = static_cast<g3::u32>(static_cast<unsigned long long>(std::time(nullptr)) + 2082844800ull);
    const g3::u64 bytes = static_cast<g3::u64>(mb) << 20;
    std::vector<g3::u8> disk(static_cast<size_t>(bytes));
    if (drivers_from) {
        FileDevice src;
        src.f = std::fopen(drivers_from, "rb");
        if (!src.f) {
            std::fprintf(stderr, "cannot read %s\n", drivers_from);
            return 1;
        }
        std::fseek(src.f, 0, SEEK_END);
        src.sectors = static_cast<g3::u64>(std::ftell(src.f)) / g3::BlockDevice::kSectorSize;
        g3::NewMacDisk nd;
        if (!nd.plan(src, bytes) || !nd.build(src, opt, disk.data(), &disk[nd.alt_mdb_offset()])) {
            std::fprintf(stderr, "cannot make the disk: %s\n", nd.why());
            return 1;
        }
        std::fclose(src.f);
    } else if (!g3::format_apm_hfs(disk.data(), disk.size(), opt)) {
        std::fprintf(stderr, "cannot format: check the volume name (1-27 characters, no ':')\n");
        return 1;
    }
    FILE* f = std::fopen(args[0], "wb");
    if (!f || std::fwrite(disk.data(), 1, disk.size(), f) != disk.size()) {
        std::fprintf(stderr, "cannot write %s\n", args[0]);
        if (f)
            std::fclose(f);
        return 1;
    }
    std::fclose(f);
    std::printf("%s: %lu MB, Apple partition map%s, HFS volume \"%s\"\n", args[0], mb,
                drivers_from ? " and drivers" : "", opt.volume_name);
    return 0;
}
