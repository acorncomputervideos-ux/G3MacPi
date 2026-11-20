// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/storage/block_device.h — a disk image as seen by emulated storage
// controllers: fixed 512-byte sectors, synchronous access.
//
// Platform code supplies the real implementation (an image file on the
// SD card, read through Circle/FatFs); RamBlockDevice serves tests and
// small RAM disks.

#ifndef G3_CORE_STORAGE_BLOCK_DEVICE_H
#define G3_CORE_STORAGE_BLOCK_DEVICE_H

#include "core/common/types.h"

namespace g3 {

class BlockDevice {
public:
    static constexpr u32 kSectorSize = 512;

    virtual u64 sector_count() const = 0;
    virtual bool read_only() const = 0;
    // Transfer `count` whole sectors starting at `lba`. Return false on an
    // I/O error or a range outside the device.
    virtual bool read(u64 lba, u32 count, u8* out) = 0;
    virtual bool write(u64 lba, u32 count, const u8* in) = 0;
    virtual bool flush() { return true; }

protected:
    ~BlockDevice() = default;
};

// A block device on caller-owned memory.
class RamBlockDevice final : public BlockDevice {
public:
    RamBlockDevice(u8* data, u64 bytes, bool read_only = false)
        : data_(data), sectors_(bytes / kSectorSize), ro_(read_only) {}

    u64 sector_count() const override { return sectors_; }
    bool read_only() const override { return ro_; }
    bool read(u64 lba, u32 count, u8* out) override
    {
        if (!in_range(lba, count))
            return false;
        const u8* src = data_ + lba * kSectorSize;
        for (u64 i = 0; i < static_cast<u64>(count) * kSectorSize; ++i)
            out[i] = src[i];
        return true;
    }
    bool write(u64 lba, u32 count, const u8* in) override
    {
        if (ro_ || !in_range(lba, count))
            return false;
        u8* dst = data_ + lba * kSectorSize;
        for (u64 i = 0; i < static_cast<u64>(count) * kSectorSize; ++i)
            dst[i] = in[i];
        return true;
    }

private:
    bool in_range(u64 lba, u32 count) const { return lba <= sectors_ && count <= sectors_ - lba; }
    u8* data_;
    u64 sectors_;
    bool ro_;
};

} // namespace g3

#endif // G3_CORE_STORAGE_BLOCK_DEVICE_H
