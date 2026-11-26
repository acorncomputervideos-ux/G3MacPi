// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/video/framebuffer.h — the emulator's display device: a
// linear framebuffer with a small register block of our own design (no
// real graphics card is imitated; our firmware and our Mac OS display
// driver program it). See docs/hardware/framebuffer.md.
//
// Pixel formats (big-endian, as PowerPC frame buffers are; the depth
// values are those Linux offb uses for Open Firmware displays):
//   depth 8  — 8 bits per pixel, index into a 256-entry palette
//   depth 15 — 16 bits per pixel, x1 R5 G5 B5
//   depth 32 — 32 bits per pixel, x8 R8 G8 B8
//
// Registers (32-bit, big-endian, at the register block):
//   0x00 ID       RO 'G3FB' (0x47334642)     0x04 VERSION  RO 1
//   0x08 WIDTH    RW staged mode             0x0C HEIGHT   RW staged mode
//   0x10 DEPTH    RW staged mode (8/15/32)   0x14 PITCH    RO bytes per row (active mode)
//   0x18 ENABLE   RW 1 = show the picture    0x1C VRAMSIZE RO bytes
//   0x20 PALINDEX RW palette index           0x24 PALDATA  RW 0x00RRGGBB; index += 1
//   0x28 STATUS   RO 1 = last APPLY accepted 0x2C APPLY    WO: staged mode -> active
//   0x30 FRAMES   RO frames shown (host side)
//   0x34 IRQ_ENABLE RW bit 0: vertical-blank interrupt on
//   0x38 IRQ_STATUS RO bit 0: a vertical blank happened; write 1 to clear
// VERSION 2 has the interrupt: every kVblPs of guest time (60 Hz, our
// choice) IRQ_STATUS bit 0 is set, and the interrupt line is high while
// IRQ_STATUS & IRQ_ENABLE bit 0 is. The timer runs only while enabled.
// The active mode only changes on APPLY, and only if the mode is valid
// (width/height 1..kMaxWidth/kMaxHeight, a supported depth, and
// pitch * height fits in VRAM). Pitch = width * bytes per pixel, rounded
// up to 32 bytes.

#ifndef G3_CORE_DEVICES_VIDEO_FRAMEBUFFER_H
#define G3_CORE_DEVICES_VIDEO_FRAMEBUFFER_H

#include "core/common/types.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"

namespace g3 {

class Framebuffer final : public MmioDevice {
public:
    static constexpr u32 kRegsSize = 0x1000;
    static constexpr u32 kId = 0x47334642u;  // 'G3FB'
    static constexpr u32 kMaxWidth = 1920, kMaxHeight = 1200;
    enum Reg : u32 {
        ID = 0x00, VERSION = 0x04, WIDTH = 0x08, HEIGHT = 0x0C, DEPTH = 0x10, PITCH = 0x14,
        ENABLE = 0x18, VRAMSIZE = 0x1C, PALINDEX = 0x20, PALDATA = 0x24, STATUS = 0x28,
        APPLY = 0x2C, FRAMES = 0x30, IRQ_ENABLE = 0x34, IRQ_STATUS = 0x38,
    };
    static constexpr u32 kVersion = 2;
    static constexpr u32 kIrqVbl = 1;
    static constexpr u64 kVblPs = 1000000000000ull / 60;  // 60 Hz

    // vram: host memory for the picture (the caller owns it).
    Framebuffer(u8* vram, u32 vram_size);

    // Maps VRAM (as RAM, for fast CPU access) and the registers.
    bool map(PhysicalBus& bus, u32 vram_base, u32 regs_base);

    // Mode setting from the host side (the firmware's default mode).
    bool set_mode(u32 width, u32 height, u32 depth);
    static u32 pitch_for(u32 width, u32 depth);
    static bool depth_supported(u32 depth) { return depth == 8 || depth == 15 || depth == 32; }

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    u32 depth() const { return depth_; }
    u32 pitch() const { return pitch_; }
    bool enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }
    u8* vram() { return vram_; }
    u32 vram_size() const { return vram_size_; }
    u32 vram_base() const { return vram_base_; }
    u32 palette(u32 i) const { return palette_[i & 255]; }
    void set_palette(u32 i, u32 rgb) { palette_[i & 255] = rgb & 0x00FFFFFFu; }

    // The vertical-blank interrupt: guest time and the interrupt line
    // (level: true while a vertical blank is pending and enabled).
    using IrqFn = void (*)(void* ctx, bool asserted);
    void attach_interrupt(Scheduler& sched, IrqFn irq, void* ctx);
    bool irq_line() const { return (irq_status_ & irq_enable_ & kIrqVbl) != 0; }

    // Host side: one 0x00RRGGBB word per pixel into dst (dst_pitch in
    // pixels), width() x height() pixels. Counts a frame.
    void convert(u32* dst, u32 dst_pitch);
    u32 frames() const { return frames_; }

    // MmioDevice (registers).
    u32 mmio_read(u32 offset, u32 size) override;
    void mmio_write(u32 offset, u32 size, u32 value) override;

private:
    u32 reg_read(u32 reg);
    void reg_write(u32 reg, u32 value);
    void update_irq();
    void arm_vbl();
    static void vbl_event(void* ctx, u64 now_ps);

    u8* vram_;
    u32 vram_size_;
    u32 vram_base_ = 0;
    u32 width_ = 0, height_ = 0, depth_ = 0, pitch_ = 0;
    u32 staged_w_ = 0, staged_h_ = 0, staged_d_ = 0;
    bool enabled_ = false;
    u32 status_ = 0;
    u32 pal_index_ = 0;
    u32 palette_[256] = {};
    u32 frames_ = 0;
    Scheduler* sched_ = nullptr;
    IrqFn irq_ = nullptr;
    void* irq_ctx_ = nullptr;
    u32 irq_enable_ = 0, irq_status_ = 0;
    bool line_ = false;
    Scheduler::Handle vbl_ = Scheduler::kInvalidHandle;
    u64 next_vbl_ = 0;
};

} // namespace g3

#endif // G3_CORE_DEVICES_VIDEO_FRAMEBUFFER_H
