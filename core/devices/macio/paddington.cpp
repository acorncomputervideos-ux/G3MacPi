// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/paddington.cpp — see paddington.h.

#include "core/devices/macio/paddington.h"

namespace g3 {

Paddington::Paddington(PhysicalBus& bus, Scheduler& scheduler, u8 revision)
    : bus_(bus), via_(scheduler), cuda_(via_, scheduler), dbdma_(bus)
{
    cfg_.init_header(0x106B, 0x0017, revision, 0xFF0000);
    cfg_.define_bar(0, kBarSize, false);
    via_.set_irq_output([](void* c, bool a) {
        static_cast<HeathrowPic*>(c)->set_input(kViaIrq, a);
    }, &pic_);
    for (u32 i = 0; i < 2; ++i) {
        ide_irq_[i] = {&pic_, kIdeIrq[i]};
        ide_dma_irq_[i] = {&pic_, kIdeDmaIrq[i]};
        ide_[i].set_irq([](void* c, bool a) {
            auto* r = static_cast<IrqRoute*>(c);
            r->pic->set_input(r->irq, a);
        }, &ide_irq_[i]);
        DbdmaChannel& ch = dbdma_.channel(kIdeDmaChannel[i]);
        ide_[i].attach_dma(&ch);
        ch.set_irq([](void* c) {  // DBDMA interrupts are pulses (edge)
            auto* r = static_cast<IrqRoute*>(c);
            r->pic->set_input(r->irq, true);
            r->pic->set_input(r->irq, false);
        }, &ide_dma_irq_[i]);
    }
}

void Paddington::config_write(u32 reg, u32 value, u32 byte_enables)
{
    cfg_.write(reg, value, byte_enables);
    if (reg == 0x04 || reg == 0x10)
        update_mapping();
}

void Paddington::unmap_cells()
{
    if (mapped_base_ == 0)
        return;
    bus_.unmap(mapped_base_ + kPicOffset);
    bus_.unmap(mapped_base_ + kViaOffset);
    bus_.unmap(mapped_base_ + kDbdmaOffset);
    bus_.unmap(mapped_base_ + kNvramOffset);
    bus_.unmap(mapped_base_ + kIdeOffset[0]);
    bus_.unmap(mapped_base_ + kIdeOffset[1]);
    mapped_base_ = 0;
}

void Paddington::update_mapping()
{
    const bool memory_enabled = (cfg_.get16(0x04) & 0x2) != 0;
    const u32 base = cfg_.get32(0x10) & ~0xFu;
    const u32 want = memory_enabled ? base : 0;
    if (want == mapped_base_)
        return;
    unmap_cells();
    if (want == 0)
        return;
    // If a cell overlaps something else on the bus, software assigned a bad
    // address; the cells then stay unmapped (as a decode conflict would).
    // Only the cells mapped here are rolled back, never the other device.
    struct Cell { u32 offset; u32 size; MmioDevice* dev; };
    const Cell cells[] = {
        {kPicOffset, HeathrowPic::kWindowSize, &pic_},
        {kDbdmaOffset, DbdmaController::kWindowSize, &dbdma_},
        {kViaOffset, Via6522::kWindowSize, &via_},
        {kNvramOffset, MacioNvram::kWindowSize, &nvram_},
        {kIdeOffset[0], MacioIde::kWindowSize, &ide_[0]},
        {kIdeOffset[1], MacioIde::kWindowSize, &ide_[1]},
    };
    constexpr u32 kCells = sizeof cells / sizeof cells[0];
    u32 mapped = 0;
    while (mapped < kCells &&
           bus_.map_mmio(want + cells[mapped].offset, cells[mapped].size, cells[mapped].dev))
        ++mapped;
    if (mapped == kCells) {
        mapped_base_ = want;
        return;
    }
    while (mapped > 0) {
        --mapped;
        bus_.unmap(want + cells[mapped].offset);
    }
}

} // namespace g3
