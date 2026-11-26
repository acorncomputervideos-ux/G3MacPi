// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/ata/cmd646.cpp — see cmd646.h.

#include "core/devices/ata/cmd646.h"

namespace g3 {

constexpr u32 Cmd646::kBarSize[5];

namespace {
// Bus master status bits (SFF-8038i).
constexpr u8 kActive = 0x01, kError = 0x02, kInterrupt = 0x04, kCapable = 0x60;
// MRDMODE (CMD646U2).
constexpr u8 kCh0Int = 0x04, kCh1Int = 0x08, kBlockCh0 = 0x10, kBlockCh1 = 0x20;
constexpr u32 kMaxPrd = 4096;  // a PRD table cannot cross a 64 KB boundary: at most 8192 entries
} // namespace

Cmd646::Cmd646(PhysicalBus& bus) : bus_(bus)
{
    cfg_.init_header(kVendor, kDevice, kRevision, kClass);
    for (u32 i = 0; i < 5; ++i)
        cfg_.define_bar(i, kBarSize[i], true);
    cfg_.set8(0x3D, 0x01);              // interrupt pin A
    cfg_.set_writable(0x3C, 1);         // interrupt line
    cfg_.set_writable(0x40, 0x40);      // device-specific registers (timing etc.): stored
    for (u32 i = 0; i < 2; ++i) {
        Channel& c = chan_[i];
        c.owner = this;
        c.index = i;
        c.ata.set_irq([](void* ctx, bool a) {
            auto* ch = static_cast<Channel*>(ctx);
            ch->owner->channel_irq(*ch, a);
        }, &c);
        c.ata.set_dma_request(dma_request, &c);
    }
    for (u32 i = 0; i < 5; ++i) {
        window_[i].owner = this;
        window_[i].bar = i;
    }
}

void Cmd646::set_irq(IrqCallback cb, void* context)
{
    irq_cb_ = cb;
    irq_ctx_ = context;
}

// --- configuration space --------------------------------------------------------------

u32 Cmd646::config_read(u32 reg)
{
    cfg_.set8(0x71, mrdmode_);  // MRDMODE is visible in configuration space too
    return cfg_.read(reg);
}

void Cmd646::config_write(u32 reg, u32 value, u32 byte_enables)
{
    reg &= 0xFC;
    if (reg == 0x70 && (byte_enables & 0x2)) {
        bm_write(1, static_cast<u8>(value >> 8));  // MRDMODE semantics, not plain storage
        byte_enables &= ~0x2u;
    }
    cfg_.write(reg, value, byte_enables);
    if (reg == 0x04 || (reg >= 0x10 && reg <= 0x20))
        update_mapping();
}

void Cmd646::update_mapping()
{
    const bool io_enabled = (cfg_.get16(0x04) & 0x1) != 0;
    for (u32 i = 0; i < 5; ++i) {
        const u32 bar = cfg_.get32(0x10 + 4 * i) & ~3u;
        const u32 want = (io_enabled && bar) ? kPciIoBase + bar : 0;
        if (want == mapped_[i])
            continue;
        if (mapped_[i])
            bus_.unmap(mapped_[i]);
        mapped_[i] = 0;
        if (want && bus_.map_mmio(want, kBarSize[i], &window_[i]))
            mapped_[i] = want;  // else: an address conflict; the window stays unmapped
    }
}

// --- I/O windows ------------------------------------------------------------------------
// The registers are little-endian bytes; a wider access covers consecutive
// byte addresses, the lowest-addressed byte being the most significant in
// the value the (big-endian) CPU sees. For the data register that keeps the
// ATA byte stream in memory order, as on the mac-io cell.

u32 Cmd646::io_read(u32 bar, u32 offset, u32 size)
{
    u8 b[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const u32 n = size > 4 ? 4 : size;
    if (bar == 0 || bar == 2) {
        AtaBus& ata = chan_[bar / 2].ata;
        if (offset == 0)
            ata.read_data(b, n);
        else if (n == 1)
            b[0] = ata.read_reg(offset);
    } else if (bar == 1 || bar == 3) {
        if (offset == 2 && n == 1)
            b[0] = chan_[bar / 2].ata.read_alt_status();
    } else {
        for (u32 i = 0; i < n; ++i)
            b[i] = bm_read(offset + i);
    }
    u32 v = 0;
    for (u32 i = 0; i < n; ++i)
        v = (v << 8) | b[i];
    return v;
}

void Cmd646::io_write(u32 bar, u32 offset, u32 size, u32 value)
{
    u8 b[4] = {};
    const u32 n = size > 4 ? 4 : size;
    for (u32 i = 0; i < n; ++i)
        b[i] = static_cast<u8>(value >> (8 * (n - 1 - i)));
    if (bar == 0 || bar == 2) {
        AtaBus& ata = chan_[bar / 2].ata;
        if (offset == 0)
            ata.write_data(b, n);
        else if (n == 1)
            ata.write_reg(offset, b[0]);
    } else if (bar == 1 || bar == 3) {
        if (offset == 2 && n == 1)
            chan_[bar / 2].ata.write_device_control(b[0]);
    } else {
        for (u32 i = 0; i < n; ++i)
            bm_write(offset + i, b[i]);
    }
}

// --- bus master -------------------------------------------------------------------------

u8 Cmd646::bm_read(u32 offset)
{
    Channel& c = chan_[(offset >> 3) & 1];
    switch (offset & 0xF) {
    case 0x0: case 0x8: return c.bm_command;
    case 0x1: return mrdmode_;
    case 0x2: case 0xA: return c.bm_status;
    case 0x3: return udidetcr0_;
    case 0x4: case 0x5: case 0x6: case 0x7:
    case 0xC: case 0xD: case 0xE: case 0xF:
        return static_cast<u8>(c.prd >> (8 * (offset & 3)));
    default: return 0;
    }
}

void Cmd646::bm_write(u32 offset, u8 v)
{
    Channel& c = chan_[(offset >> 3) & 1];
    switch (offset & 0xF) {
    case 0x0: case 0x8: {
        const bool was = c.bm_command & 1;
        c.bm_command = v & 0x09;  // start, read/write
        if ((v & 1) && !was) {
            c.bm_status |= kActive;
            run_dma(c);
        } else if (!(v & 1) && was) {
            c.bm_status &= static_cast<u8>(~kActive);  // stopping aborts the transfer
        }
        return;
    }
    case 0x1:
        mrdmode_ = static_cast<u8>(mrdmode_ & ~(v & (kCh0Int | kCh1Int)));      // write 1 to clear
        mrdmode_ = static_cast<u8>((mrdmode_ & ~(kBlockCh0 | kBlockCh1)) | (v & (kBlockCh0 | kBlockCh1)));
        update_irq();
        return;
    case 0x2: case 0xA:
        c.bm_status = static_cast<u8>((c.bm_status & ~(v & (kError | kInterrupt)) & ~kCapable) |
                                      (v & kCapable));
        return;
    case 0x3:
        udidetcr0_ = v;
        return;
    case 0x4: case 0x5: case 0x6: case 0x7:
    case 0xC: case 0xD: case 0xE: case 0xF: {
        const u32 shift = 8 * (offset & 3);
        c.prd = (c.prd & ~(0xFFu << shift)) | (static_cast<u32>(v) << shift);
        c.prd &= ~3u;  // dword aligned
        return;
    }
    default:
        return;
    }
}

void Cmd646::dma_request(void* ctx)
{
    auto* c = static_cast<Channel*>(ctx);
    c->owner->run_dma(*c);
}

void Cmd646::run_dma(Channel& c)
{
    // Runs when both the engine is started and the drive has a DMA command.
    if (!(c.bm_command & 1) || !(c.bm_status & kActive) || !c.ata.dma_pending())
        return;
    const bool to_memory = (c.bm_command & 0x08) != 0;
    u32 entry = c.prd;
    for (u32 k = 0; k < kMaxPrd; ++k, entry += 8) {
        const u8* p = bus_.host_pointer(entry, 8);
        if (!p) {
            c.bm_status = static_cast<u8>((c.bm_status | kError) & ~kActive);
            return;
        }
        const u32 addr = static_cast<u32>(p[0]) | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24);
        u32 count = static_cast<u32>(p[4]) | (p[5] << 8);
        if (count == 0)
            count = 0x10000;
        const bool last = (p[7] & 0x80) != 0;
        u8* mem = bus_.host_pointer(addr & ~1u, count);
        if (!mem) {
            c.bm_status = static_cast<u8>((c.bm_status | kError) & ~kActive);
            return;
        }
        const u32 moved = to_memory ? c.ata.dma_read(mem, count) : c.ata.dma_write(mem, count);
        if (to_memory)
            bus_.notify_write(addr & ~1u, moved);
        if (moved < count) {
            // The drive finished before the table did: the engine stays
            // active until software stops it (SFF-8038i).
            return;
        }
        if (last) {
            c.bm_status &= static_cast<u8>(~kActive);
            return;
        }
    }
}

// --- interrupts -------------------------------------------------------------------------

void Cmd646::channel_irq(Channel& c, bool asserted)
{
    c.intrq = asserted;
    const u8 bit = c.index ? kCh1Int : kCh0Int;
    if (asserted) {
        c.bm_status |= kInterrupt;
        mrdmode_ |= bit;
    } else {
        mrdmode_ = static_cast<u8>(mrdmode_ & ~bit);
    }
    update_irq();
}

void Cmd646::update_irq()
{
    const bool now = (chan_[0].intrq && !(mrdmode_ & kBlockCh0)) || (chan_[1].intrq && !(mrdmode_ & kBlockCh1));
    if (now != irq_) {
        irq_ = now;
        if (irq_cb_)
            irq_cb_(irq_ctx_, irq_);
    }
}

} // namespace g3
