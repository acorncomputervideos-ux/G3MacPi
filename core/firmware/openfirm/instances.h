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
#include "core/firmware/openfirm/device_tree.h"
#include "core/storage/block_device.h"

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
// read with seek + read). Partition and file arguments are not
// interpreted yet: every instance sees the whole disk.
class DiskDriver final : public PackageDriver {
public:
    explicit DiskDriver(BlockDevice& disk) : disk_(disk) {}
    bool open(u32 slot, const char* args) override;
    s32 read(u32 slot, u8* buf, u32 len) override;
    s32 write(u32 slot, const u8* buf, u32 len) override;
    bool seek(u32 slot, u64 pos) override;
    u64 position(u32 slot) const { return pos_[slot % InstanceTable::kMaxInstances]; }

private:
    BlockDevice& disk_;
    u64 pos_[InstanceTable::kMaxInstances] = {};
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
