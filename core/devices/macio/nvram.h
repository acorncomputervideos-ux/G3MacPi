// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/nvram.h — the mac-io's 8 KB non-volatile RAM.
//
// Facts (docs/hardware/nvram.md):
//  - 8 KB (Linux NVRAM_SIZE 0x2000; QEMU MACIO_NVRAM_SIZE 0x2000).
//  - "Direct" access: byte n is at window offset n * stride, where the
//    stride is the region size / 8 KB (Linux nvram_mult). QEMU's Old World
//    mac-io places it at BAR + 0x60000 with a stride of 16 (it_shift 4),
//    i.e. a 0x20000-byte window. Placement and stride for the B&W: to verify.
//  - Wider accesses behave as consecutive byte accesses, big-endian
//    lanes (QEMU impl max_access_size 1); with stride 16 these usually hit
//    the same NVRAM byte. Linux only uses in_8/out_8.
//  - Initial contents of a never-written NVRAM: zeros (an assumption; our
//    firmware formats it).
//
// The contents persist across emulator runs through data()/load(): the
// platform layer saves them (e.g. to the SD card) when dirty() is set.

#ifndef G3_CORE_DEVICES_MACIO_NVRAM_H
#define G3_CORE_DEVICES_MACIO_NVRAM_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"

namespace g3 {

class MacioNvram final : public MmioDevice {
public:
    static constexpr u32 kSize = 0x2000;
    static constexpr u32 kStride = 16;
    static constexpr u32 kWindowSize = kSize * kStride;

    MacioNvram();

    // Persistence.
    const u8* data() const { return data_; }
    void load(const u8* bytes, u32 len);  // copies min(len, kSize) bytes
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    u8 byte(u32 index) const { return data_[index & (kSize - 1)]; }

    // MmioDevice
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    u8 data_[kSize];
    bool dirty_ = false;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_NVRAM_H
