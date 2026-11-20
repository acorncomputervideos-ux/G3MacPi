// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/mem/physical_bus.h — the guest's physical address space.
//
// Regions (RAM, ROM, MMIO devices) are mapped at fixed physical addresses
// (docs/02-architecture.md 3.2). The CPU reaches memory through this bus via
// ppc::MemoryInterface; DMA-capable devices use host_pointer() for RAM.
//
// - RAM: read/write, stored in guest (big-endian) byte order.
// - ROM: read-only; writes are ignored and counted (real ROM is not writable
//   and does not raise bus errors on the Macs we emulate — to be verified per
//   machine, see docs/hardware/physical-bus.md).
// - MMIO: forwarded to an MmioDevice with the offset inside the region and
//   the access size (1, 2 or 4). Values are passed as the CPU sees them
//   (big-endian interpretation); a device with little-endian registers
//   swaps itself.
// - Unmapped: either a bus error (the CPU takes a machine check) or reads
//   return all ones and writes are dropped — selectable, because the
//   behaviour depends on the host bridge and the address range.
//
// An access straddling two regions (or a region edge) is split into bytes.
// Freestanding: fixed number of regions, no allocation.

#ifndef G3_CORE_MEM_PHYSICAL_BUS_H
#define G3_CORE_MEM_PHYSICAL_BUS_H

#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"

namespace g3 {

class MmioDevice {
public:
    // size is 1, 2 or 4; offset is relative to the start of the mapping.
    virtual u32 mmio_read(u32 offset, u32 size) = 0;
    virtual void mmio_write(u32 offset, u32 size, u32 value) = 0;

protected:
    ~MmioDevice() = default;
};

class PhysicalBus final : public ppc::MemoryInterface {
public:
    enum class UnmappedPolicy : u8 { BusError, AllOnes };
    static constexpr u32 kMaxRegions = 32;

    // Mapping functions return false if the region overlaps an existing
    // one, has size 0, wraps past 4 GB, or the table is full.
    bool map_ram(u32 base, u8* data, u32 size);
    bool map_rom(u32 base, const u8* data, u32 size);
    bool map_mmio(u32 base, u32 size, MmioDevice* device);

    // Removes the region starting exactly at 'base'. Returns false if there
    // is none. Used when software moves a PCI device's registers (BAR write).
    bool unmap(u32 base);

    void set_unmapped_policy(UnmappedPolicy p) { unmapped_policy_ = p; }

    // Host pointer to 'len' bytes of RAM at physical address 'pa', or
    // nullptr if the range is not entirely inside one RAM region.
    u8* host_pointer(u32 pa, u32 len);

    // ppc::MemoryInterface
    bool read8(u32 addr, u8& value) override;
    bool read16(u32 addr, u16& value) override;
    bool read32(u32 addr, u32& value) override;
    bool write8(u32 addr, u8 value) override;
    bool write16(u32 addr, u16 value) override;
    bool write32(u32 addr, u32 value) override;
    // The RAM region at physical address 0 (main memory), if mapped.
    u8* direct_window(u32& base, u32& size) override;

    // RAM writes: the observer hears of every write to RAM made through the
    // bus, and devices that write RAM through host_pointer() (DMA) report
    // it with notify_write(). Used by the CPU's decoded-block cache.
    using WriteObserver = void (*)(void* context, u32 pa, u32 len);
    void set_write_observer(WriteObserver cb, void* context)
    {
        observer_ = cb;
        observer_ctx_ = context;
    }
    void notify_write(u32 pa, u32 len)
    {
        if (observer_ && len)
            observer_(observer_ctx_, pa, len);
    }

    // Debugging: told of every device-register access (after it, with the
    // value read or written); nullptr = none.
    using MmioTrace = void (*)(void* context, u32 addr, u32 size, bool write, u32 value);
    void set_mmio_trace(MmioTrace cb, void* context)
    {
        mmio_trace_ = cb;
        mmio_trace_ctx_ = context;
    }

    // Diagnostics.
    u64 unmapped_accesses() const { return unmapped_count_; }
    u32 last_unmapped_address() const { return last_unmapped_; }
    u64 rom_writes_ignored() const { return rom_write_count_; }

private:
    enum class Kind : u8 { Ram, Rom, Mmio };
    struct Region {
        u32 base;
        u32 size;
        Kind kind;
        u8* data;             // RAM/ROM (ROM is only read through it)
        MmioDevice* device;   // MMIO
    };

    MmioTrace mmio_trace_ = nullptr;
    void* mmio_trace_ctx_ = nullptr;
    WriteObserver observer_ = nullptr;
    void* observer_ctx_ = nullptr;

    bool add(const Region& r);
    // Region containing [addr, addr + len), or nullptr.
    Region* find(u32 addr, u32 len);
    bool access(u32 addr, u32 size, bool write, u32& value);
    bool access_split(u32 addr, u32 size, bool write, u32& value);
    bool unmapped(u32 addr, u32 size, bool write, u32& value);

    Region regions_[kMaxRegions] = {};
    u32 count_ = 0;
    Region* last_hit_ = nullptr;
    UnmappedPolicy unmapped_policy_ = UnmappedPolicy::BusError;
    u64 unmapped_count_ = 0;
    u32 last_unmapped_ = 0;
    u64 rom_write_count_ = 0;
};

} // namespace g3

#endif // G3_CORE_MEM_PHYSICAL_BUS_H
