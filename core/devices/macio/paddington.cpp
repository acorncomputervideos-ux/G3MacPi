// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/paddington.cpp — see paddington.h.

#include "core/devices/macio/paddington.h"

namespace g3 {

Paddington::Paddington(PhysicalBus& bus, Scheduler& scheduler, u8 revision)
    : bus_(bus), via_(scheduler), cuda_(via_, scheduler), dbdma_(bus), sound_(scheduler, MacioDavbus::Codec::Screamer), bmac_(scheduler)
{
    cfg_.init_header(0x106B, 0x0017, revision, 0xFF0000);
    cfg_.define_bar(0, kBarSize, false);
    via_.set_irq_output([](void* c, bool a) {
        static_cast<HeathrowPic*>(c)->set_input(kViaIrq, a);
    }, &pic_);
    escc_.set_irq([](void* c, u32 ch, bool a) { static_cast<HeathrowPic*>(c)->set_input(kEsccIrq[ch & 1], a); },
                  &pic_);
    mesh_.set_irq([](void* c, bool a) { static_cast<HeathrowPic*>(c)->set_input(kMeshIrq, a); }, &pic_);
    bmac_.set_irq([](void* c, bool a) { static_cast<HeathrowPic*>(c)->set_input(kBmacIrq, a); }, &pic_);
    sound_.attach_dma(&dbdma_.channel(kSoundDmaChannel[0]), &dbdma_.channel(kSoundDmaChannel[1]));
    bmac_.attach_dma(&dbdma_.channel(kBmacDmaChannel[0]), &dbdma_.channel(kBmacDmaChannel[1]));
    for (u32 i = 0; i < 2; ++i) {
        sound_dma_irq_[i] = {&pic_, kSoundDmaIrq[i]};
        bmac_dma_irq_[i] = {&pic_, kBmacDmaIrq[i]};
        const auto raise = [](void* c) {  // DMA lines stay up until the PIC's ack clears them
            auto* r = static_cast<IrqRoute*>(c);
            r->pic->set_input(r->irq, true);
        };
        dbdma_.channel(kSoundDmaChannel[i]).set_irq(raise, &sound_dma_irq_[i]);
        dbdma_.channel(kBmacDmaChannel[i]).set_irq(raise, &bmac_dma_irq_[i]);
        escc_.attach_dma(i, &dbdma_.channel(kEsccDmaChannel[i][0]), &dbdma_.channel(kEsccDmaChannel[i][1]));
        for (u32 d = 0; d < 2; ++d) {
            escc_dma_irq_[i][d] = {&pic_, kEsccDmaIrq[i][d]};
            dbdma_.channel(kEsccDmaChannel[i][d]).set_irq(raise, &escc_dma_irq_[i][d]);
        }
    }
    for (u32 i = 0; i < 2; ++i) {
        ide_irq_[i] = {&pic_, kIdeIrq[i]};
        ide_dma_irq_[i] = {&pic_, kIdeDmaIrq[i]};
        ide_[i].set_irq([](void* c, bool a) {
            auto* r = static_cast<IrqRoute*>(c);
            r->pic->set_input(r->irq, a);
        }, &ide_irq_[i]);
        DbdmaChannel& ch = dbdma_.channel(kIdeDmaChannel[i]);
        ide_[i].attach_dma(&ch);
        ch.set_irq([](void* c) {  // DMA lines stay up until the PIC's ack clears them
            auto* r = static_cast<IrqRoute*>(c);
            r->pic->set_input(r->irq, true);
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
    bus_.unmap(mapped_base_ + kMeshOffset);
    bus_.unmap(mapped_base_ + kEsccOffset);
    bus_.unmap(mapped_base_ + kSoundOffset);
    bus_.unmap(mapped_base_ + kBmacOffset);
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
        {kMeshOffset, MacioMesh::kWindowSize, &mesh_},
        {kEsccOffset, MacioEscc::kWindowSize, &escc_},
        {kSoundOffset, MacioDavbus::kWindowSize, &sound_},
        {kBmacOffset, MacioBmac::kWindowSize, &bmac_},
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
