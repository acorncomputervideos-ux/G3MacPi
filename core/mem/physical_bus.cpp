// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/mem/physical_bus.cpp — see physical_bus.h.

#include "core/mem/physical_bus.h"

#include "core/common/endian.h"

namespace g3 {

bool PhysicalBus::add(const Region& r)
{
    if (r.size == 0 || count_ >= kMaxRegions)
        return false;
    const u64 end = static_cast<u64>(r.base) + r.size;
    if (end > 0x100000000ull)
        return false;
    for (u32 i = 0; i < count_; ++i) {
        const u64 b = regions_[i].base;
        const u64 e = b + regions_[i].size;
        if (r.base < e && b < end)
            return false;  // overlap
    }
    regions_[count_++] = r;
    last_hit_ = nullptr;
    return true;
}

bool PhysicalBus::unmap(u32 base)
{
    for (u32 i = 0; i < count_; ++i) {
        if (regions_[i].base != base)
            continue;
        regions_[i] = regions_[count_ - 1];
        --count_;
        last_hit_ = nullptr;
        return true;
    }
    return false;
}

bool PhysicalBus::map_ram(u32 base, u8* data, u32 size)
{
    return data && add(Region{base, size, Kind::Ram, data, nullptr});
}

bool PhysicalBus::map_rom(u32 base, const u8* data, u32 size)
{
    // Stored as non-const; the bus never writes through ROM mappings.
    return data && add(Region{base, size, Kind::Rom, const_cast<u8*>(data), nullptr});
}

bool PhysicalBus::map_mmio(u32 base, u32 size, MmioDevice* device)
{
    return device && add(Region{base, size, Kind::Mmio, nullptr, device});
}

PhysicalBus::Region* PhysicalBus::find(u32 addr, u32 len)
{
    auto contains = [&](const Region* r) {
        // addr - base < size and the access fits (no overflow for large addresses)
        const u32 off = addr - r->base;
        return addr >= r->base && off < r->size && len <= r->size - off;
    };
    if (last_hit_ && contains(last_hit_))
        return last_hit_;
    for (u32 i = 0; i < count_; ++i) {
        if (contains(&regions_[i])) {
            last_hit_ = &regions_[i];
            return last_hit_;
        }
    }
    return nullptr;
}

u8* PhysicalBus::host_pointer(u32 pa, u32 len)
{
    Region* r = find(pa, len);
    if (!r || r->kind != Kind::Ram)
        return nullptr;
    return r->data + (pa - r->base);
}

bool PhysicalBus::unmapped(u32 addr, u32 size, bool write, u32& value)
{
    ++unmapped_count_;
    last_unmapped_ = addr;
    if (unmapped_policy_ == UnmappedPolicy::BusError)
        return false;
    if (!write)
        value = size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFFu : 0xFFu);
    return true;
}

bool PhysicalBus::access_split(u32 addr, u32 size, bool write, u32& value)
{
    // Byte by byte, most significant byte first (big-endian).
    u32 result = 0;
    for (u32 i = 0; i < size; ++i) {
        const u32 shift = 8 * (size - 1 - i);
        u32 b = write ? ((value >> shift) & 0xFF) : 0;
        if (!access(addr + i, 1, write, b))
            return false;
        result |= (b & 0xFF) << shift;
    }
    if (!write)
        value = result;
    return true;
}

bool PhysicalBus::access(u32 addr, u32 size, bool write, u32& value)
{
    Region* r = find(addr, size);
    if (!r) {
        // Not inside one region. If any byte is mapped, the access straddles
        // a region edge: do it byte by byte (each byte goes to its own region
        // or is handled as unmapped). Otherwise nothing is there.
        if (size > 1) {
            for (u32 i = 0; i < size; ++i)
                if (find(addr + i, 1))
                    return access_split(addr, size, write, value);
        }
        return unmapped(addr, size, write, value);
    }
    const u32 off = addr - r->base;
    switch (r->kind) {
    case Kind::Ram:
    case Kind::Rom: {
        u8* p = r->data + off;
        if (write) {
            if (r->kind == Kind::Rom) {
                ++rom_write_count_;
                return true;
            }
            if (size == 4) store_be32(p, value);
            else if (size == 2) store_be16(p, static_cast<u16>(value));
            else *p = static_cast<u8>(value);
            notify_write(addr, size);
        } else {
            value = size == 4 ? load_be32(p) : (size == 2 ? load_be16(p) : *p);
        }
        return true;
    }
    case Kind::Mmio:
        if (write)
            r->device->mmio_write(off, size, value);
        else
            value = r->device->mmio_read(off, size);
        return true;
    }
    return false;
}

bool PhysicalBus::read8(u32 a, u8& v)
{
    u32 x;
    if (!access(a, 1, false, x))
        return false;
    v = static_cast<u8>(x);
    return true;
}

bool PhysicalBus::read16(u32 a, u16& v)
{
    u32 x;
    if (!access(a, 2, false, x))
        return false;
    v = static_cast<u16>(x);
    return true;
}

bool PhysicalBus::read32(u32 a, u32& v) { return access(a, 4, false, v); }

bool PhysicalBus::write8(u32 a, u8 v)
{
    u32 x = v;
    return access(a, 1, true, x);
}

bool PhysicalBus::write16(u32 a, u16 v)
{
    u32 x = v;
    return access(a, 2, true, x);
}

bool PhysicalBus::write32(u32 a, u32 v) { return access(a, 4, true, v); }

u8* PhysicalBus::direct_window(u32& base, u32& size)
{
    for (u32 i = 0; i < count_; ++i) {
        const Region& r = regions_[i];
        if (r.kind == Kind::Ram && r.base == 0) {
            base = r.base;
            size = r.size;
            return r.data;
        }
    }
    return nullptr;
}

} // namespace g3
