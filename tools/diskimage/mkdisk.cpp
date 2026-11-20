// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/diskimage/mkdisk.cpp — g3_mkdisk: writes a new, empty Mac disk image
// (Apple partition map + one HFS volume) for the emulated ATA disk.
//
//   g3_mkdisk <image> <size in MB> [volume name]
//
// The volume holds no drivers and no System Folder: Mac OS mounts it, but
// cannot start up from it. docs/storage/disk-format.md.

#include "core/storage/disk_format.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: g3_mkdisk <image> <size in MB> [volume name]\n");
        return 2;
    }
    const unsigned long mb = std::strtoul(argv[2], nullptr, 10);
    if (mb < 1 || mb > 2000) {
        std::fprintf(stderr, "size must be 1-2000 MB (HFS: 65535 allocation blocks)\n");
        return 2;
    }
    g3::HfsFormatOptions opt;
    opt.volume_name = argc > 3 ? argv[3] : "Untitled";
    // Mac OS dates count seconds from 1904-01-01; Unix time from 1970.
    opt.date = static_cast<g3::u32>(static_cast<unsigned long long>(std::time(nullptr)) + 2082844800ull);
    std::vector<g3::u8> disk(static_cast<size_t>(mb) * 1024 * 1024);
    if (!g3::format_apm_hfs(disk.data(), disk.size(), opt)) {
        std::fprintf(stderr, "cannot format: check the volume name (1-27 characters, no ':')\n");
        return 1;
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!f || std::fwrite(disk.data(), 1, disk.size(), f) != disk.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        if (f)
            std::fclose(f);
        return 1;
    }
    std::fclose(f);
    std::printf("%s: %lu MB, Apple partition map, HFS volume \"%s\"\n", argv[1], mb, opt.volume_name);
    return 0;
}
