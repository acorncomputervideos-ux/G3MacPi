// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/instances.cpp — see instances.h.

#include "core/firmware/openfirm/instances.h"

#include "core/common/cstring.h"

namespace g3::of {

bool InstanceTable::bind(Phandle package, PackageDriver* driver)
{
    for (u32 i = 0; i < binding_count_; ++i)
        if (bindings_[i].package == package) {
            bindings_[i].driver = driver;
            return true;
        }
    if (binding_count_ >= kMaxBindings || package == kNone)
        return false;
    bindings_[binding_count_++] = {package, driver};
    return true;
}

PackageDriver* InstanceTable::driver_for(Phandle package) const
{
    for (u32 i = 0; i < binding_count_; ++i)
        if (bindings_[i].package == package)
            return bindings_[i].driver;
    return nullptr;
}

Ihandle InstanceTable::open(Phandle package, const char* args)
{
    PackageDriver* d = driver_for(package);
    if (!d)
        return 0;  // no driver: the package has no "open" method
    u32 s = 0;
    while (s < kMaxInstances && inst_[s].used)
        ++s;
    if (s == kMaxInstances)
        return 0;
    Instance& in = inst_[s];
    u32 n = 0;
    if (args)
        for (; n < kMaxArgs && args[n] != '\0'; ++n)
            in.args[n] = args[n];
    in.args[n] = '\0';
    if (args && args[n] != '\0')
        return 0;  // arguments too long
    if (!d->open(s, in.args))
        return 0;
    in.used = true;
    in.package = package;
    in.driver = d;
    return kBase + s;
}

bool InstanceTable::valid(Ihandle ih) const
{
    return ih >= kBase && ih - kBase < kMaxInstances && inst_[ih - kBase].used;
}

bool InstanceTable::close(Ihandle ih)
{
    if (!valid(ih))
        return false;
    Instance& in = inst_[ih - kBase];
    in.driver->close(ih - kBase);
    in.used = false;
    return true;
}

Phandle InstanceTable::package(Ihandle ih) const { return valid(ih) ? inst_[ih - kBase].package : kNone; }
PackageDriver* InstanceTable::driver(Ihandle ih) const { return valid(ih) ? inst_[ih - kBase].driver : nullptr; }
const char* InstanceTable::args(Ihandle ih) const { return valid(ih) ? inst_[ih - kBase].args : ""; }

u32 InstanceTable::open_count() const
{
    u32 n = 0;
    for (const Instance& in : inst_)
        n += in.used ? 1 : 0;
    return n;
}

// --- DiskDriver ----------------------------------------------------------------------

namespace {
constexpr u32 kSector = BlockDevice::kSectorSize;
}

bool DiskDriver::open(u32 slot, const char* args)
{
    const u32 s = slot % InstanceTable::kMaxInstances;
    pos_[s] = 0;
    base_[s] = 0;
    size_[s] = disk_.sector_count() * kSector;
    part_[s] = 0;
    if (args == nullptr || args[0] < '0' || args[0] > '9')
        return true;  // no partition: the whole disk
    u32 n = 0;
    for (const char* p = args; *p >= '0' && *p <= '9'; ++p) {
        n = n * 10 + static_cast<u32>(*p - '0');
        if (n > ApplePartitionMap::kMaxPartitions)
            return false;
    }
    if (!map_.read(disk_))
        return false;
    const ApmPartition* p = map_.partition(n);
    if (!p)
        return false;
    const u64 disk_bytes = disk_.sector_count() * kSector;
    const u64 start = static_cast<u64>(p->start) * kSector;
    u64 len = static_cast<u64>(p->blocks) * kSector;
    if (start >= disk_bytes)
        return false;
    if (len > disk_bytes - start)
        len = disk_bytes - start;  // clip a map entry that runs past the image
    base_[s] = start;
    size_[s] = len;
    part_[s] = n;
    file_[s] = false;
    const char* comma = args;
    while (*comma && *comma != ',')
        ++comma;
    if (*comma == ',' && comma[1] != '\0')
        return open_file(s, comma + 1);
    return true;
}

bool DiskDriver::mount_partition(u32 s)
{
    if (fs_base_ == base_[s] && fs_.mounted())
        return true;
    window_.set(base_[s] / kSector, size_[s] / kSector);
    fs_base_ = ~0ull;
    if (!fs_.mount(window_))
        return false;
    fs_base_ = base_[s];
    return true;
}

bool DiskDriver::open_file(u32 s, const char* spec)
{
    if (!mount_partition(s))
        return false;
    u32 folder = HfsVolume::kRootFolder;
    const char* p = spec;
    if (p[0] == '\\' && p[1] == '\\') {
        folder = fs_.blessed_folder();
        p += 2;
        if (folder == 0)
            return false;  // volume isn't bootable
    } else if (p[0] == '\\') {
        p += 1;
    }
    HfsEntry e;
    bool have_file = false;
    while (*p) {
        if (*p == ':') {  // :TYPE — exactly four characters
            const char* t = p + 1;
            if (!t[0] || !t[1] || !t[2] || !t[3] || t[4])
                return false;
            const u32 type = (static_cast<u32>(static_cast<u8>(t[0])) << 24) |
                             (static_cast<u32>(static_cast<u8>(t[1])) << 16) |
                             (static_cast<u32>(static_cast<u8>(t[2])) << 8) | static_cast<u8>(t[3]);
            if (!fs_.find_by_type(folder, type, e))
                return false;
            have_file = true;
            break;
        }
        char name[256];
        u32 n = 0;
        while (*p && *p != '\\' && *p != ':' && n < sizeof name - 1)
            name[n++] = *p++;
        name[n] = '\0';
        if (*p == '\\')
            ++p;
        if (n == 0)
            continue;
        if (!fs_.lookup(folder, name, e))
            return false;
        if (e.folder) {
            folder = e.id;
            have_file = false;
        } else {
            if (*p)
                return false;  // a file in the middle of the path
            have_file = true;
        }
    }
    if (!have_file)
        return false;  // opening a folder isn't supported
    file_[s] = true;
    fork_[s] = e.data;
    size_[s] = e.data.logical_size;
    pos_[s] = 0;
    return true;
}

bool DiskDriver::seek(u32 slot, u64 pos)
{
    if (pos > size_[slot % InstanceTable::kMaxInstances])
        return false;
    pos_[slot % InstanceTable::kMaxInstances] = pos;
    return true;
}

s32 DiskDriver::read(u32 slot, u8* buf, u32 len)
{
    const u32 sl = slot % InstanceTable::kMaxInstances;
    u64& pos = pos_[sl];
    const u64 size = size_[sl];
    if (file_[sl]) {
        if (!mount_partition(sl))
            return -1;
        const s64 n = fs_.read(fork_[sl], pos, buf, len > 0x7FFFFFFFu ? 0x7FFFFFFFu : len);
        if (n > 0)
            pos += static_cast<u64>(n);
        return static_cast<s32>(n);
    }
    if (len > 0x7FFFFFFFu)
        len = 0x7FFFFFFFu;
    u32 done = 0;
    while (done < len && pos < size) {
        const u64 lba = (base_[sl] + pos) / kSector;
        const u32 off = static_cast<u32>((base_[sl] + pos) % kSector);
        u32 n = kSector - off;
        if (n > len - done)
            n = len - done;
        if (!disk_.read(lba, 1, sector_))
            return done ? static_cast<s32>(done) : -1;
        for (u32 i = 0; i < n; ++i)
            buf[done + i] = sector_[off + i];
        done += n;
        pos += n;
    }
    return static_cast<s32>(done);
}

s32 DiskDriver::write(u32 slot, const u8* buf, u32 len)
{
    const u32 sl = slot % InstanceTable::kMaxInstances;
    u64& pos = pos_[sl];
    const u64 size = size_[sl];
    if (disk_.read_only() || file_[sl])
        return -1;  // files are read-only
    if (len > 0x7FFFFFFFu)
        len = 0x7FFFFFFFu;
    u32 done = 0;
    while (done < len && pos < size) {
        const u64 lba = (base_[sl] + pos) / kSector;
        const u32 off = static_cast<u32>((base_[sl] + pos) % kSector);
        u32 n = kSector - off;
        if (n > len - done)
            n = len - done;
        if (n != kSector && !disk_.read(lba, 1, sector_))  // read-modify-write
            return done ? static_cast<s32>(done) : -1;
        for (u32 i = 0; i < n; ++i)
            sector_[off + i] = buf[done + i];
        if (!disk_.write(lba, 1, sector_))
            return done ? static_cast<s32>(done) : -1;
        done += n;
        pos += n;
    }
    return static_cast<s32>(done);
}

} // namespace g3::of

namespace g3::of {

s32 NvramDriver::read(u32 slot, u8* buf, u32 len)
{
    u32& pos = pos_[slot % InstanceTable::kMaxInstances];
    u32 n = 0;
    for (; n < len && pos < size_; ++n, ++pos)
        buf[n] = get_(ctx_, pos);
    return static_cast<s32>(n);
}

s32 NvramDriver::write(u32 slot, const u8* buf, u32 len)
{
    u32& pos = pos_[slot % InstanceTable::kMaxInstances];
    u32 n = 0;
    for (; n < len && pos < size_; ++n, ++pos)
        put_(ctx_, pos, buf[n]);
    return static_cast<s32>(n);
}

bool NvramDriver::seek(u32 slot, u64 pos)
{
    if (pos > size_)
        return false;
    pos_[slot % InstanceTable::kMaxInstances] = static_cast<u32>(pos);
    return true;
}

bool NvramDriver::method(u32, const char* name, forth::Forth& f)
{
    if (!cstr::equal(name, "size"))
        return false;
    f.push(size_);
    return true;
}

} // namespace g3::of
