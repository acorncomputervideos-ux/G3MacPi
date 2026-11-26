// SPDX-License-Identifier: GPL-3.0-or-later
//
// tools/hfs/hfs_tool.cpp — lists and extracts files from an HFS or HFS Plus
// volume image (bare, or the first Apple_HFS partition of a partitioned
// disk) with the emulator's own reader (core/storage/hfs_volume).
//   g3_hfs IMAGE ls                    every folder and file, recursively
//   g3_hfs IMAGE get PATH OUT [rsrc]   one fork of a file (path with ':')

#include "core/storage/apple_partition_map.h"
#include "core/storage/block_device.h"
#include "core/storage/hfs_volume.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace g3;

namespace {

std::string name_of(const HfsVolume& v, const HfsEntry& e)
{
    std::string s;
    for (u32 i = 0; i < e.name_len; ++i) {
        const u16 c = e.name[i];
        if (c >= 0x20 && c < 0x7F)
            s.push_back(static_cast<char>(c));
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            s += buf;
        }
    }
    (void)v;
    return s;
}

std::string fourcc(u32 v)
{
    std::string s;
    for (int i = 3; i >= 0; --i) {
        const char c = static_cast<char>(v >> (8 * i));
        s.push_back(c >= 0x20 && c < 0x7F ? c : '.');
    }
    return s;
}

void list_folder(HfsVolume& v, u32 folder, const std::string& prefix)
{
    std::vector<HfsEntry> entries;
    v.list(folder, [](void* ctx, const HfsEntry& e) {
        static_cast<std::vector<HfsEntry>*>(ctx)->push_back(e);
        return true;
    }, &entries);
    for (const HfsEntry& e : entries) {
        const std::string path = prefix + name_of(v, e);
        if (e.folder) {
            std::printf("%-60s  folder %u%s\n", (path + ":").c_str(), e.id, e.id == v.blessed_folder() ? " (blessed)" : "");
            list_folder(v, e.id, path + ":");
        } else {
            std::printf("%-60s  %s/%s data %llu rsrc %llu\n", path.c_str(), fourcc(e.type).c_str(),
                        fourcc(e.creator).c_str(), static_cast<unsigned long long>(e.data.logical_size),
                        static_cast<unsigned long long>(e.rsrc.logical_size));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s IMAGE ls | get PATH OUT [rsrc]\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<u8> img;
    for (int c; (c = std::fgetc(f)) != EOF;)
        img.push_back(static_cast<u8>(c));
    std::fclose(f);
    img.resize((img.size() + 511) & ~size_t{511});
    RamBlockDevice disk(img.data(), img.size(), true);

    // A partitioned disk: use the first Apple_HFS partition.
    BlockWindow window(disk, 0, disk.sector_count());
    BlockDevice* dev = &disk;
    static ApplePartitionMap map;
    if (map.read(disk)) {
        const u32 n = map.find_type("Apple_HFS");
        if (n) {
            window.set(map.partition(n)->start, map.partition(n)->blocks);
            dev = &window;
        }
    }
    static HfsVolume v;
    if (!v.mount(*dev)) {
        std::fprintf(stderr, "not an HFS or HFS Plus volume\n");
        return 1;
    }
    if (std::strcmp(argv[2], "ls") == 0) {
        std::printf("%s volume, block size %u, blessed folder %u\n", v.classic() ? "HFS" : "HFS Plus", v.block_size(),
                    v.blessed_folder());
        list_folder(v, HfsVolume::kRootFolder, "");
        return 0;
    }
    if (std::strcmp(argv[2], "get") == 0 && argc >= 5) {
        // Walk the ':'-separated path from the root folder.
        std::string path = argv[3];
        u32 folder = HfsVolume::kRootFolder;
        HfsEntry e;
        size_t pos = 0;
        while (true) {
            const size_t colon = path.find(':', pos);
            const std::string part = path.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
            if (!v.lookup(folder, part.c_str(), e)) {
                std::fprintf(stderr, "not found: %s\n", part.c_str());
                return 1;
            }
            if (colon == std::string::npos)
                break;
            folder = e.id;
            pos = colon + 1;
        }
        const HfsFork& fork = argc >= 6 && std::strcmp(argv[5], "rsrc") == 0 ? e.rsrc : e.data;
        std::vector<u8> out(fork.logical_size);
        u64 done = 0;
        while (done < out.size()) {
            const s64 n = v.read(fork, done, out.data() + done, static_cast<u32>(out.size() - done));
            if (n <= 0) {
                std::fprintf(stderr, "read error at %llu\n", static_cast<unsigned long long>(done));
                return 1;
            }
            done += static_cast<u64>(n);
        }
        FILE* o = std::fopen(argv[4], "wb");
        if (!o || std::fwrite(out.data(), 1, out.size(), o) != out.size()) {
            std::fprintf(stderr, "cannot write %s\n", argv[4]);
            return 1;
        }
        std::fclose(o);
        std::printf("%llu bytes\n", static_cast<unsigned long long>(out.size()));
        return 0;
    }
    std::fprintf(stderr, "unknown command\n");
    return 2;
}
