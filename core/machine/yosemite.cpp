// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/yosemite.cpp — see yosemite.h.

#include "core/machine/yosemite.h"

namespace g3 {

YosemiteMachine::YosemiteMachine(const YosemiteConfig& c)
    : cpu_(bus_),
      loop_(cpu_, sched_, CpuTiming{c.cpu_hz, c.bus_hz / 4, 1}),
      bridge_(PciToPciBridge::dec21154()),
      macio_(bus_, sched_)
{
    bool ok = c.ram && c.ram_size > 0 && bus_.map_ram(0, c.ram, c.ram_size);
    ram_size_ = c.ram_size;
    cpu_hz_ = c.cpu_hz;
    bus_hz_ = c.bus_hz;
    if (ok && c.rom)
        ok = bus_.map_rom(kRomBase, c.rom, c.rom_size);
    ok = ok && grackle_.attach(bus_) && grackle_.pci_bus().attach(kBridgeDevfn, &bridge_) &&
         bridge_.secondary_bus().attach(kMacIoDevfn, &macio_);
    macio_.pic().set_output([](void* cpu, bool a) {
        static_cast<ppc::Ppc750*>(cpu)->set_external_interrupt(a);
    }, &cpu_);
    macio_.cuda().set_rtc(c.rtc_seconds_since_1904);
    ok = ok && adb_.attach(&keyboard_) && adb_.attach(&mouse_);
    macio_.cuda().attach_adb(&adb_);
    ok_ = ok;
}

} // namespace g3
