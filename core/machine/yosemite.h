// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/yosemite.h — the Power Macintosh G3 (Blue & White),
// board "Yosemite", PowerMac1,1: wiring of all emulated parts.
//
// Topology (docs/hardware/yosemite.md; items marked * are from emulator
// sources only and must be verified with a real device tree):
//   RAM at 0, boot ROM at 0xFFF00000 (1 MB)
//   PCI bus 0: 00:00.0 Grackle (MPC106) host bridge
//              00:0D.0 DEC 21154 PCI-to-PCI bridge *
//   behind the bridge: device 5 Paddington mac-io *
//   VIA (Cuda) -> interrupt controller input 18 * -> CPU external interrupt
//   ADB (via Cuda): keyboard at address 2, mouse at address 3
//   CPU 350 MHz, bus 100 MHz, time base = bus / 4 (25 MHz)
//
// RAM and ROM buffers are supplied by the caller (the core does not
// allocate).

#ifndef G3_CORE_MACHINE_YOSEMITE_H
#define G3_CORE_MACHINE_YOSEMITE_H

#include "core/cpu/ppc750/ppc750.h"
#include "core/devices/adb/adb.h"
#include "core/devices/macio/paddington.h"
#include "core/devices/pci/grackle.h"
#include "core/devices/pci/pci_bridge.h"
#include "core/devices/video/framebuffer.h"
#include "core/machine/execution_loop.h"
#include "core/mem/physical_bus.h"
#include "core/devices/ata/cmd646.h"
#include "core/cpu/ppc750/jit_worker.h"
#include "core/sched/scheduler.h"

namespace g3 {

struct YosemiteConfig {
    u8* ram = nullptr;
    u32 ram_size = 0;               // bytes
    const u8* rom = nullptr;        // optional boot ROM image
    u32 rom_size = 0;
    u64 cpu_hz = 350000000;
    u64 bus_hz = 100000000;
    u32 rtc_seconds_since_1904 = 0;
    // Video memory for the display device (optional; at most kMaxVram).
    u8* vram = nullptr;
    u32 vram_size = 0;
    // Memory for the CPU's decoded-block cache (docs/cpu/block-cache.md;
    // a few MB). None: the CPU interprets every instruction.
    u8* jit_memory = nullptr;
    u32 jit_size = 0;
    // Executable memory for generated AArch64 code (docs/cpu/jit-a64.md;
    // needs jit_memory too). Without a runner the code is called directly,
    // which only works on an AArch64 host.
    u32* jit_code = nullptr;
    u32 jit_code_words = 0;
    ppc::Ppc750::JitRunner jit_runner = nullptr;
    void* jit_runner_context = nullptr;
    // Compile on another core (core/cpu/ppc750/jit_worker.h): the front end
    // then polls jit_worker() there. Needs jit_code.
    bool jit_background = false;
};

class YosemiteMachine {
public:
    static constexpr u32 kRomBase = 0xFFF00000u;
    static constexpr u32 kBridgeDevfn = 0x0D << 3;   // * to verify
    static constexpr u32 kMacIoDevfn = 0x05 << 3;    // * to verify (behind the bridge)
    // The CMD646 hard-disk controller: 01:01.0 (Linux lspci on a B&W), its
    // INTA wired to Paddington input 26 (Linux log; DingusPPC).
    static constexpr u32 kCmd646Devfn = 0x01 << 3;
    static constexpr u32 kCmd646Irq = 26;
    // The display device (our own; docs/hardware/framebuffer.md): video
    // memory and registers in PCI memory space, outside any PCI BAR.
    static constexpr u32 kVramBase = 0x90000000u;
    static constexpr u32 kMaxVram = 16u << 20;
    static constexpr u32 kDisplayRegs = 0x91000000u;
    // Its node sits on the Grackle bus as PCI device 0x10 (our choice of
    // slot): Mac OS maps it from the node's assigned-addresses. It has no
    // configuration space. docs/hardware/framebuffer.md.
    static constexpr u32 kDisplayDevfn = 0x10 << 3;

    explicit YosemiteMachine(const YosemiteConfig& config);

    // False if the configuration could not be wired (e.g. no RAM).
    bool ok() const { return ok_; }

    void run_for(u64 picoseconds) { loop_.run_for(picoseconds); }

    ppc::Ppc750& cpu() { return cpu_; }
    bool jit_enabled() const { return jit_; }  // generated code in use
    // The background compiler to poll on another core, or nullptr.
    ppc::JitWorker* jit_worker() { return jit_ && jit_background_ ? &jit_worker_ : nullptr; }
    u32 ram_size() const { return ram_size_; }
    u64 cpu_hz() const { return cpu_hz_; }
    u64 bus_hz() const { return bus_hz_; }
    PhysicalBus& bus() { return bus_; }
    Scheduler& scheduler() { return sched_; }
    Grackle& grackle() { return grackle_; }
    PciToPciBridge& bridge() { return bridge_; }
    Paddington& macio() { return macio_; }
    Cmd646& cmd646() { return cmd646_; }
    AdbKeyboard& keyboard() { return keyboard_; }
    AdbMouse& mouse() { return mouse_; }
    Framebuffer& display() { return display_; }
    bool has_display() const { return has_display_; }

private:
    // Construction order matters: the bus and scheduler come first.
    Scheduler sched_;
    PhysicalBus bus_;
    ppc::Ppc750 cpu_;
    ppc::BlockCache blocks_;
    bool jit_ = false;
    bool jit_background_ = false;
    ppc::JitWorker jit_worker_{cpu_};
    ExecutionLoop loop_;
    Grackle grackle_;
    PciToPciBridge bridge_;
    Paddington macio_;
    Cmd646 cmd646_;
    AdbBus adb_;
    AdbKeyboard keyboard_;
    AdbMouse mouse_;
    Framebuffer display_;
    bool has_display_ = false;
    bool ok_ = false;
    u32 ram_size_ = 0;
    u64 cpu_hz_ = 0, bus_hz_ = 0;
};

} // namespace g3

#endif // G3_CORE_MACHINE_YOSEMITE_H
