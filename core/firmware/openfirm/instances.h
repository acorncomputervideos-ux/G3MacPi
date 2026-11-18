// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/instances.h — open device instances (ihandles)
// and the C++ drivers behind device packages.
//
// Facts (docs/firmware/open-firmware.md; IEEE 1275 §6.3.2):
//  - open ( device-specifier -- ihandle ): opens the package with the
//    specifier's arguments; 0 on failure.
//  - close ( ihandle -- ); read/write ( ihandle addr len -- actual ),
//    actual = -1 on error; seek ( ihandle pos.hi pos.lo -- status ),
//    status 0 ok, -1 error; instance-to-package ( ihandle -- phandle ).
//
// ihandles are 0x20000000 + slot (any non-zero value is allowed), so
// they never collide with phandles.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_INSTANCES_H
#define G3_CORE_FIRMWARE_OPENFIRM_INSTANCES_H

#include "core/common/types.h"
#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/device_tree.h"
#include "core/storage/apple_partition_map.h"
#include "core/storage/block_device.h"
#include "core/storage/hfs_volume.h"

namespace g3::of {

using Ihandle = u32;

// A device package's methods, implemented in C++. `slot` identifies the
// instance (0 .. InstanceTable::kMaxInstances - 1) for per-instance state.
class PackageDriver {
public:
    virtual bool open(u32 slot, const char* args) = 0;
    virtual void close(u32 /*slot*/) {}
    virtual s32 read(u32 /*slot*/, u8* /*buf*/, u32 /*len*/) { return -1; }
    virtual s32 write(u32 /*slot*/, const u8* /*buf*/, u32 /*len*/) { return -1; }
    virtual bool seek(u32 /*slot*/, u64 /*pos*/) { return false; }
    // Other methods, called by $call-method: take arguments from and leave
    // results on the Forth stack. Returns false if the method doesn't exist.
    virtual bool method(u32 /*slot*/, const char* /*name*/, forth::Forth& /*f*/) { return false; }

protected:
    ~PackageDriver() = default;
};

class InstanceTable {
public:
    static constexpr u32 kMaxInstances = 32;
    static constexpr u32 kMaxBindings = 64;
    static constexpr u32 kMaxArgs = 63;

    bool bind(Phandle package, PackageDriver* driver);
    PackageDriver* driver_for(Phandle package) const;

    Ihandle open(Phandle package, const char* args);  // 0 on failure
    bool close(Ihandle ih);

    bool valid(Ihandle ih) const;
    Phandle package(Ihandle ih) const;           // kNone if invalid
    PackageDriver* driver(Ihandle ih) const;     // nullptr if invalid
    u32 slot(Ihandle ih) const { return ih - kBase; }
    const char* args(Ihandle ih) const;          // "" if none/invalid
    u32 open_count() const;

private:
    static constexpr u32 kBase = 0x20000000u;
    struct Binding { Phandle package; PackageDriver* driver; };
    struct Instance { bool used; Phandle package; PackageDriver* driver; char args[kMaxArgs + 1]; };
    Binding bindings_[kMaxBindings] = {};
    u32 binding_count_ = 0;
    Instance inst_[kMaxInstances] = {};
};

// Byte-addressed access to a disk image (IEEE 1275 "block" devices are
// read with seek + read). Open arguments "N" or "N,file" select
// partition N of the Apple Partition Map: the instance then sees only
// that partition (offset 0 = its first byte). Without a leading number
// the instance sees the whole disk.
//
// A file part opens a file of the partition's HFS Plus volume instead,
// read-only (docs/firmware/open-firmware.md, "File paths"):
//   \\       the blessed System Folder;  \  the root folder;
//   name\name  path components;            :TYPE  first file of Finder type TYPE.
// e.g. "9,\\:tbxi" = the file of type 'tbxi' in partition 9's blessed folder.
class DiskDriver final : public PackageDriver {
public:
    explicit DiskDriver(BlockDevice& disk) : disk_(disk) {}
    bool open(u32 slot, const char* args) override;
    s32 read(u32 slot, u8* buf, u32 len) override;
    s32 write(u32 slot, const u8* buf, u32 len) override;
    bool seek(u32 slot, u64 pos) override;
    u64 position(u32 slot) const { return pos_[slot % InstanceTable::kMaxInstances]; }
    u64 window_size(u32 slot) const { return size_[slot % InstanceTable::kMaxInstances]; }
    u32 partition(u32 slot) const { return part_[slot % InstanceTable::kMaxInstances]; }
    bool is_file(u32 slot) const { return file_[slot % InstanceTable::kMaxInstances]; }

private:
    bool mount_partition(u32 slot);
    bool open_file(u32 slot, const char* spec);

    BlockDevice& disk_;
    ApplePartitionMap map_;
    BlockWindow window_{disk_, 0, 0};
    HfsVolume fs_;
    u64 fs_base_ = ~0ull;                           // window the volume is mounted on
    bool file_[InstanceTable::kMaxInstances] = {};
    HfsFork fork_[InstanceTable::kMaxInstances];
    u64 pos_[InstanceTable::kMaxInstances] = {};
    u64 base_[InstanceTable::kMaxInstances] = {};   // bytes
    u64 size_[InstanceTable::kMaxInstances] = {};   // bytes
    u32 part_[InstanceTable::kMaxInstances] = {};   // 0 = whole disk
    u8 sector_[BlockDevice::kSectorSize] = {};
};

// The console (stdout): written text goes to a sink, e.g. the boot log.
// Reading returns 0 bytes (no input yet).
class ConsoleDriver final : public PackageDriver {
public:
    using Sink = void (*)(void* context, const char* text, u32 len);
    void set_sink(Sink s, void* ctx) { sink_ = s; ctx_ = ctx; }
    bool open(u32, const char*) override { return true; }
    s32 read(u32, u8*, u32) override { return 0; }
    s32 write(u32, const u8* buf, u32 len) override
    {
        if (sink_)
            sink_(ctx_, reinterpret_cast<const char*>(buf), len);
        return static_cast<s32>(len);
    }

private:
    Sink sink_ = nullptr;
    void* ctx_ = nullptr;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_INSTANCES_H
