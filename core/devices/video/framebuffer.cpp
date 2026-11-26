// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/video/framebuffer.cpp — see framebuffer.h.

#include "core/devices/video/framebuffer.h"

namespace g3 {

Framebuffer::Framebuffer(u8* vram, u32 vram_size) : vram_(vram), vram_size_(vram_size)
{
    // A grey ramp as the power-on palette (index 0 black, 255 white).
    for (u32 i = 0; i < 256; ++i)
        palette_[i] = (i << 16) | (i << 8) | i;
}

bool Framebuffer::map(PhysicalBus& bus, u32 vram_base, u32 regs_base)
{
    if (!vram_ || vram_size_ == 0)
        return false;
    if (!bus.map_ram(vram_base, vram_, vram_size_))
        return false;
    if (!bus.map_mmio(regs_base, kRegsSize, this)) {
        bus.unmap(vram_base);
        return false;
    }
    vram_base_ = vram_base;
    return true;
}

u32 Framebuffer::pitch_for(u32 width, u32 depth)
{
    const u32 bytes = depth == 8 ? 1 : depth == 15 ? 2 : 4;
    return (width * bytes + 31) & ~31u;
}

bool Framebuffer::set_mode(u32 w, u32 h, u32 d)
{
    if (w == 0 || h == 0 || w > kMaxWidth || h > kMaxHeight || !depth_supported(d))
        return false;
    const u32 p = pitch_for(w, d);
    if (static_cast<u64>(p) * h > vram_size_)
        return false;
    width_ = w;
    height_ = h;
    depth_ = d;
    pitch_ = p;
    staged_w_ = w;
    staged_h_ = h;
    staged_d_ = d;
    return true;
}

u32 Framebuffer::reg_read(u32 reg)
{
    switch (reg) {
    case ID: return kId;
    case VERSION: return kVersion;
    case WIDTH: return staged_w_;
    case HEIGHT: return staged_h_;
    case DEPTH: return staged_d_;
    case PITCH: return pitch_;
    case ENABLE: return enabled_ ? 1 : 0;
    case VRAMSIZE: return vram_size_;
    case PALINDEX: return pal_index_;
    case PALDATA: {
        const u32 v = palette_[pal_index_];
        pal_index_ = (pal_index_ + 1) & 255;
        return v;
    }
    case STATUS: return status_;
    case FRAMES: return frames_;
    case IRQ_ENABLE: return irq_enable_;
    case IRQ_STATUS: return irq_status_;
    default: return 0;
    }
}

void Framebuffer::reg_write(u32 reg, u32 v)
{
    switch (reg) {
    case WIDTH: staged_w_ = v; return;
    case HEIGHT: staged_h_ = v; return;
    case DEPTH: staged_d_ = v; return;
    case ENABLE: enabled_ = (v & 1) != 0; return;
    case PALINDEX: pal_index_ = v & 255; return;
    case PALDATA:
        palette_[pal_index_] = v & 0x00FFFFFFu;
        pal_index_ = (pal_index_ + 1) & 255;
        return;
    case APPLY:
        // set_mode checks everything before changing anything, so a
        // rejected mode leaves the active mode (and the staged values) as they were.
        status_ = set_mode(staged_w_, staged_h_, staged_d_) ? 1 : 0;
        return;
    case IRQ_ENABLE:
        irq_enable_ = v & kIrqVbl;
        if (irq_enable_)
            arm_vbl();
        else if (sched_ && vbl_ != Scheduler::kInvalidHandle) {
            sched_->cancel(vbl_);
            vbl_ = Scheduler::kInvalidHandle;
        }
        update_irq();
        return;
    case IRQ_STATUS:
        irq_status_ &= ~(v & kIrqVbl);
        update_irq();
        return;
    default: return;  // read-only registers ignore writes
    }
}

void Framebuffer::attach_interrupt(Scheduler& sched, IrqFn irq, void* ctx)
{
    sched_ = &sched;
    irq_ = irq;
    irq_ctx_ = ctx;
}

void Framebuffer::update_irq()
{
    const bool now = irq_line();
    if (now != line_) {
        line_ = now;
        if (irq_)
            irq_(irq_ctx_, now);
    }
}

// Vertical blanks come every kVblPs from when the interrupt was first
// enabled; the next one is planned from the last, so the rate stays 60 Hz
// however late an event runs.
void Framebuffer::arm_vbl()
{
    if (!sched_ || vbl_ != Scheduler::kInvalidHandle)
        return;
    const u64 now = sched_->now();
    if (next_vbl_ <= now)
        next_vbl_ = now + kVblPs;
    vbl_ = sched_->schedule_at(next_vbl_, vbl_event, this);
}

void Framebuffer::vbl_event(void* ctx, u64 now_ps)
{
    auto* fb = static_cast<Framebuffer*>(ctx);
    fb->vbl_ = Scheduler::kInvalidHandle;
    fb->irq_status_ |= kIrqVbl;
    fb->update_irq();
    fb->next_vbl_ += kVblPs;
    if (fb->next_vbl_ <= now_ps)
        fb->next_vbl_ = now_ps + kVblPs;
    if (fb->irq_enable_)
        fb->arm_vbl();
}

u32 Framebuffer::mmio_read(u32 offset, u32 size)
{
    // Registers are 32-bit: narrower reads take the matching bytes (big-endian).
    const u32 v = reg_read(offset & ~3u);
    if (size == 4)
        return v;
    const u32 shift = 8 * (4 - size - (offset & 3));
    return (v >> shift) & (size == 1 ? 0xFFu : 0xFFFFu);
}

void Framebuffer::mmio_write(u32 offset, u32 size, u32 value)
{
    if (size == 4 && (offset & 3) == 0)
        reg_write(offset, value);  // narrower writes are ignored
}

void Framebuffer::convert(u32* dst, u32 dst_pitch)
{
    ++frames_;
    for (u32 y = 0; y < height_; ++y) {
        const u8* row = vram_ + static_cast<size_t>(y) * pitch_;
        u32* out = dst + static_cast<size_t>(y) * dst_pitch;
        switch (depth_) {
        case 8:
            for (u32 x = 0; x < width_; ++x)
                out[x] = palette_[row[x]];
            break;
        case 15:
            for (u32 x = 0; x < width_; ++x) {
                const u32 p = (static_cast<u32>(row[2 * x]) << 8) | row[2 * x + 1];
                const u32 r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
                // Expand 5 bits to 8 by repeating the top bits.
                out[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
            }
            break;
        case 32:
            for (u32 x = 0; x < width_; ++x)
                out[x] = (static_cast<u32>(row[4 * x + 1]) << 16) | (static_cast<u32>(row[4 * x + 2]) << 8) |
                         row[4 * x + 3];
            break;
        default:
            break;
        }
    }
}

} // namespace g3
