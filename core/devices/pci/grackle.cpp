// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/pci/grackle.cpp — see grackle.h.

#include "core/devices/pci/grackle.h"

#include "core/devices/le_register.h"

namespace g3 {

namespace {
constexpr u32 kEnable = 0x80000000u;
constexpr u32 bus_of(u32 a) { return (a >> 16) & 0xFF; }
constexpr u32 devfn_of(u32 a) { return (a >> 8) & 0xFF; }
constexpr u32 reg_of(u32 a) { return a & 0xFC; }
} // namespace

Grackle::Grackle(u8 revision)
{
    cfg_.init_header(0x1057, 0x0002, revision, 0x060000);
    // 0x80-0xFF: MPC106 memory controller / PCI interface configuration
    // (MCCR1-4, PICR1/2, ...). Plain read/write storage until modelled.
    cfg_.set_writable(0x80, 0x80);
    address_port_.g = this;
    data_port_.g = this;
}

bool Grackle::attach(PhysicalBus& bus)
{
    return bus.map_mmio(kConfigAddressBase, kPortWindow, &address_port_) &&
           bus.map_mmio(kConfigDataBase, kPortWindow, &data_port_) && bus0_.attach(0, this);
}

u32 Grackle::AddressPort::mmio_read(u32 offset, u32 size)
{
    return le_reg_read(g->config_address_, offset & 3, size);
}

void Grackle::AddressPort::mmio_write(u32 offset, u32 size, u32 value)
{
    // Bits 1-0 select the configuration cycle type in PCI configuration
    // mechanism #1 and are not part of the register number; they are kept
    // at zero here (Linux's GRACKLE_CFA() never sets them).
    g->config_address_ = le_reg_write(g->config_address_, offset & 3, size, value) & ~0x3u;
}

u32 Grackle::DataPort::mmio_read(u32 offset, u32 size) { return g->data_read(offset, size); }

void Grackle::DataPort::mmio_write(u32 offset, u32 size, u32 value)
{
    g->data_write(offset, size, value);
}

u32 Grackle::data_read(u32 offset, u32 size)
{
    const u32 a = config_address_;
    u32 dword = 0xFFFFFFFFu;  // disabled or no device: master abort
    if ((a & kEnable) && bus_of(a) == 0)
        dword = bus0_.config_read(devfn_of(a), reg_of(a));
    return le_reg_read(dword, offset & 3, size);
}

void Grackle::data_write(u32 offset, u32 size, u32 value)
{
    const u32 a = config_address_;
    if (!(a & kEnable) || bus_of(a) != 0)
        return;
    // Merge into a dword image and pass the byte enables of the bytes written.
    const u32 lane = offset & 3;
    u32 enables = 0;
    for (u32 i = 0; i < size && lane + i < 4; ++i)
        enables |= 1u << (lane + i);
    const u32 dword = le_reg_write(0, lane, size, value);
    bus0_.config_write(devfn_of(a), reg_of(a), dword, enables);
}

} // namespace g3
