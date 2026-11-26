// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/yosemite.cpp — see yosemite.h.

#include "core/machine/yosemite.h"

namespace g3 {

YosemiteMachine::YosemiteMachine(const YosemiteConfig& c)
    : cpu_(bus_),
      loop_(cpu_, sched_, CpuTiming{c.cpu_hz, c.bus_hz / 4, 1}),
      bridge_(PciToPciBridge::dec21154()),
      macio_(bus_, sched_), cmd646_(bus_), display_(c.vram, c.vram_size)
{
    bool ok = c.ram && c.ram_size > 0 && bus_.map_ram(0, c.ram, c.ram_size);
    ram_size_ = c.ram_size;
    if (ok && c.jit_memory && blocks_.init(c.jit_memory, c.jit_size, c.ram_size)) {
        cpu_.set_block_cache(&blocks_);
        bus_.set_write_observer([](void* cache, u32 pa, u32 len) {
            static_cast<ppc::BlockCache*>(cache)->note_write(pa, len);
        }, &blocks_);
        if (c.jit_code)
            jit_ = cpu_.set_jit(c.jit_code, c.jit_code_words, c.jit_runner, c.jit_runner_context);
        if (jit_ && c.jit_background) {
            cpu_.set_jit_worker(&jit_worker_);
            jit_background_ = true;
        }
    }
    cpu_hz_ = c.cpu_hz;
    bus_hz_ = c.bus_hz;
    if (ok && c.rom)
        ok = bus_.map_rom(kRomBase, c.rom, c.rom_size);
    ok = ok && grackle_.attach(bus_) && grackle_.pci_bus().attach(kBridgeDevfn, &bridge_) &&
         bridge_.secondary_bus().attach(kMacIoDevfn, &macio_) &&
         bridge_.secondary_bus().attach(kCmd646Devfn, &cmd646_);
    cmd646_.set_irq([](void* pic, bool a) {
        static_cast<HeathrowPic*>(pic)->set_input(kCmd646Irq, a);
    }, &macio_.pic());
    display_.attach_interrupt(sched_, [](void* pic, bool a) {
        static_cast<HeathrowPic*>(pic)->set_input(kDisplayIrq, a);
    }, &macio_.pic());
    macio_.pic().set_output([](void* cpu, bool a) {
        static_cast<ppc::Ppc750*>(cpu)->set_external_interrupt(a);
    }, &cpu_);
    macio_.cuda().set_rtc(c.rtc_seconds_since_1904);
    ok = ok && adb_.attach(&keyboard_) && adb_.attach(&mouse_);
    macio_.cuda().attach_adb(&adb_);
    if (ok && c.vram) {
        has_display_ = c.vram_size <= kMaxVram && display_.map(bus_, kVramBase, kDisplayRegs);
        ok = has_display_;
    }
    ok_ = ok;
}

} // namespace g3
