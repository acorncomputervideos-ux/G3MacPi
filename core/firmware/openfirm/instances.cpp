// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/instances.cpp — see instances.h.

#include "core/firmware/openfirm/instances.h"

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

bool DiskDriver::open(u32 slot, const char*)
{
    pos_[slot % InstanceTable::kMaxInstances] = 0;
    return true;
}

bool DiskDriver::seek(u32 slot, u64 pos)
{
    if (pos > disk_.sector_count() * kSector)
        return false;
    pos_[slot % InstanceTable::kMaxInstances] = pos;
    return true;
}

s32 DiskDriver::read(u32 slot, u8* buf, u32 len)
{
    u64& pos = pos_[slot % InstanceTable::kMaxInstances];
    const u64 size = disk_.sector_count() * kSector;
    if (len > 0x7FFFFFFFu)
        len = 0x7FFFFFFFu;
    u32 done = 0;
    while (done < len && pos < size) {
        const u64 lba = pos / kSector;
        const u32 off = static_cast<u32>(pos % kSector);
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
    u64& pos = pos_[slot % InstanceTable::kMaxInstances];
    const u64 size = disk_.sector_count() * kSector;
    if (disk_.read_only())
        return -1;
    if (len > 0x7FFFFFFFu)
        len = 0x7FFFFFFFu;
    u32 done = 0;
    while (done < len && pos < size) {
        const u64 lba = pos / kSector;
        const u32 off = static_cast<u32>(pos % kSector);
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
