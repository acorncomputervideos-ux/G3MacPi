// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end interrupt path: a device raises an edge interrupt at a
// scheduled guest time -> Heathrow PIC -> CPU external interrupt -> guest
// handler reads the event register and acknowledges it with a byte-reversed
// store (the usual way PowerPC software accesses little-endian registers),
// then returns with rfi.

#include "core/cpu/ppc750/ppc750.h"
#include "core/devices/macio/heathrow_pic.h"
#include "core/machine/execution_loop.h"
#include "core/mem/physical_bus.h"
#include "core/sched/scheduler.h"
#include "tests/framework/g3test.h"
#include "tests/unit/cpu/ppc_asm.h"

using namespace g3;
using namespace asm_ppc;

TEST_CASE("devices/interrupt_path/device_to_cpu_handler_and_back")
{
    static u8 ram[0x10000];
    PhysicalBus bus;
    HeathrowPic pic;
    REQUIRE(bus.map_ram(0, ram, sizeof ram));
    REQUIRE(bus.map_mmio(0xF3000000u, HeathrowPic::kWindowSize, &pic));
    ppc::Ppc750 cpu(bus);
    pic.set_output([](void* c, bool a) { static_cast<ppc::Ppc750*>(c)->set_external_interrupt(a); },
                   &cpu);

    // Main program: enable IRQ 12 in the PIC, then spin.
    //   lis r3,0xF300 ; li r6,0x24 ; li r5,0x1000 (bit 12) ; stwbrx r5,r3,r6 ; b .
    const u32 main_code[] = {lis(3, 0xF300), li(6, 0x24), li(5, 0x1000), stwbrx(5, 3, 6), b(0)};
    // Handler at 0x500: count, read event (lwbrx), ack it, rfi.
    //   addi r7,r7,1 ; li r6,0x20 ; lwbrx r8,r3,r6 ; li r6,0x28 ; stwbrx r8,r3,r6 ; rfi
    const u32 handler[] = {addi(7, 7, 1), li(6, 0x20), lwbrx(8, 3, 6), li(6, 0x28), stwbrx(8, 3, 6),
                           rfi()};
    u32 a = 0x1000;
    for (u32 w : main_code) { bus.write32(a, w); a += 4; }
    a = 0x500;
    for (u32 w : handler) { bus.write32(a, w); a += 4; }

    cpu.state().pc = 0x1000;
    cpu.state().msr = ppc::msr::EE;
    // As firmware would: keep the decrementer from firing during the test
    // (DEC = 0 would expire after one time-base tick and take the
    // decrementer exception at 0x900, which has no handler here).
    cpu.state().spr[ppc::spr::DEC] = 0x7FFFFFFFu;
    Scheduler sched;
    ExecutionLoop loop(cpu, sched, CpuTiming{350000000, 25000000, 1});

    // The device fires twice: at 1 us and at 2 us (edge pulses).
    struct Device {
        HeathrowPic* pic;
        static void pulse(void* c, u64)
        {
            auto* d = static_cast<Device*>(c);
            d->pic->set_input(12, true);
            d->pic->set_input(12, false);
        }
    };
    Device dev{&pic};
    sched.schedule_at(1000000, Device::pulse, &dev);
    sched.schedule_at(2000000, Device::pulse, &dev);
    loop.run_for(3000000);

    CHECK_EQ(cpu.state().gpr[7], 2u);           // handler ran once per pulse
    CHECK_EQ(cpu.state().gpr[8], 1u << 12);     // it saw IRQ 12 in the event register
    CHECK(!pic.output());                       // acknowledged
    CHECK_EQ(cpu.state().pc, 0x1010u);          // back in the main loop (b .)
    CHECK_EQ(cpu.state().msr, ppc::msr::EE);    // rfi restored MSR[EE]
}
