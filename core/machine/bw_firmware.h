// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/bw_firmware.h — our high-level Open Firmware, installed in a
// Blue & White machine: what Apple's boot ROM firmware would do at power-on.
//
// power-on (the constructor):
//  - assigns the PCI resources: the bridge's bus numbers, the mac-io BAR;
//  - lays out firmware memory at the top of RAM (docs/firmware/machine-boot.md);
//  - builds the B&W device tree from the machine's configuration;
//  - binds the disk images and the console to their device nodes, and
//    sets /chosen stdin/stdout;
//  - installs the client-interface entry and hooks the CPU.
// command("boot hd:2,\\:tbxi") then runs Open Firmware text; when a client
// program is started (`go`), the CPU is set up to run it, and the caller
// runs the machine.

#ifndef G3_CORE_MACHINE_BW_FIRMWARE_H
#define G3_CORE_MACHINE_BW_FIRMWARE_H

#include "core/firmware/forth/forth.h"
#include "core/firmware/openfirm/bw_device_tree.h"
#include "core/firmware/openfirm/display_driver.h"
#include "core/firmware/openfirm/firmware.h"
#include "core/firmware/openfirm/forth_words.h"
#include "core/firmware/openfirm/instances.h"
#include "core/firmware/openfirm/memory_map.h"
#include "core/machine/yosemite.h"
#include "core/storage/block_device.h"

namespace g3 {

class BwFirmware {
public:
    struct Config {
        BlockDevice* disk[2] = {nullptr, nullptr};  // IDE0 and IDE1 master disks
        u32 macio_base = 0x80800000u;
        u32 load_base = 0x00400000u;
        // Power-on display mode (if the machine has a display).
        u32 width = 640, height = 480, depth = 8;
    };
    // Firmware memory at the top of RAM.
    struct Layout {
        u32 base;          // start of the firmware region (= end of free RAM)
        u32 ci_entry;      // client-interface trap word
        u32 forth_data;    // Forth data space
        u32 forth_size;
        u32 scratch;       // property values handed to Forth
        u32 scratch_size;
        u32 stack_top;     // client stack (grows down)
        static Layout for_ram(u32 ram_size);
    };
    static constexpr u32 kFirmwareSize = 0x00100000u;  // 1 MB
    static constexpr u32 kMinRam = 8u << 20;

    BwFirmware(YosemiteMachine& m, const Config& c);
    bool ok() const { return ok_; }

    // Output written to the console (stdout) goes here.
    void set_console(of::ConsoleDriver::Sink sink, void* ctx) { console_.set_sink(sink, ctx); }

    // Runs Open Firmware user-interface text. False on a Forth error
    // (then forth().error_text() says why).
    bool command(const char* text);

    bool client_started() const { return started_; }
    u32 client_entry() const { return entry_; }
    const Layout& layout() const { return layout_; }

    of::DeviceTree& tree() { return tree_; }
    const of::BwNodes& nodes() const { return nodes_; }
    of::MemoryMap& memory() { return memory_; }
    of::InstanceTable& instances() { return inst_; }
    forth::Forth& forth() { return forth_; }
    of::ClientInterface& client() { return fw_.client(); }

private:
    static void go(void* ctx, u32 entry);
    static u32 clock_ms(void* ctx);

    YosemiteMachine& m_;
    Layout layout_;
    bool ok_ = false;
    bool started_ = false;
    u32 entry_ = 0;
    of::DeviceTree tree_;
    of::BwNodes nodes_{};
    of::MemoryMap memory_;
    of::InstanceTable inst_;
    forth::Forth forth_;
    of::FirmwareWords words_;
    of::Firmware fw_;
    of::ConsoleDriver console_;
    RamBlockDevice no_disk_{nullptr, 0};
    of::DiskDriver disk_[2];
    of::DisplayDriver display_;
};

} // namespace g3

#endif // G3_CORE_MACHINE_BW_FIRMWARE_H
