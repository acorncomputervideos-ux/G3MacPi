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
#include "core/firmware/openfirm/guest_view.h"
#include "core/firmware/openfirm/mmu.h"
#include "core/firmware/openfirm/rtas.h"
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
        // Our Mac OS display driver (guest/ndrv, a PEF container), placed in the
        // display node's "driver,AAPL,MacOS,PowerPC" property, which is where
        // Mac OS finds a device's native driver. Optional; the caller owns it.
        const u8* display_driver = nullptr;
        u32 display_driver_size = 0;
        // The real-time clock at power-on (RTAS get-time-of-day): seconds
        // since 1970-01-01 UTC. The Pi side will supply the real time.
        u64 rtc_seconds = 978307200ull;  // 2001-01-01 00:00:00
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
        u32 htab;          // hashed page table, just below the firmware region
        u32 htab_size;     // RAM / 128 (PEM 7.6.1.1's guideline), 64 KB .. 32 MB, aligned to it
        static Layout for_ram(u32 ram_size);
    };
    static constexpr u32 kFirmwareSize = 0x00100000u;  // 1 MB
    static constexpr u32 kVectorsEnd = 0x4000u;         // exception vectors, never handed out
    static constexpr u32 kMaxRam = 512u << 20;          // two BATs (docs/firmware/virtual-mode.md)
    static constexpr u32 kMinRam = 8u << 20;

    BwFirmware(YosemiteMachine& m, const Config& c);
    bool ok() const { return ok_; }

    // Output written to the console (stdout) goes here.
    void set_console(of::ConsoleDriver::Sink sink, void* ctx) { console_.set_sink(sink, ctx); }

    // Runs Open Firmware user-interface text. False on a Forth error
    // (then forth().error_text() says why).
    bool command(const char* text);

    // Puts a file at load-base as `load` would (see FirmwareWords::place_file).
    bool place_file(const u8* data, u32 len, const char* path) { return words_.place_file(data, len, path); }

    bool client_started() const { return started_; }
    bool client_exited() const { return fw_.client_exited(); }
    const of::Firmware& firmware() const { return fw_; }
    // RTAS power-off / system-reboot was called.
    bool powered_off() const { return powered_off_; }
    bool reboot_requested() const { return reboot_; }
    of::Rtas& rtas() { return rtas_; }
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
    static bool sync_mmu(void* ctx);
    static of::Rtas::Hooks rtas_hooks(BwFirmware* self);
    bool apply_mmu();                   // BATs + page table from translations_
    bool in_bat_window(u32 va) const;
    static u32 clock_ms(void* ctx);

    YosemiteMachine& m_;
    Layout layout_;
    bool ok_ = false;
    bool started_ = false;
    u32 entry_ = 0;
    of::Ihandle stdout_ = 0;
    of::DeviceTree tree_;
    of::BwNodes nodes_{};
    of::MemoryMap memory_;
    of::InstanceTable inst_;
    of::GuestView view_;              // before forth_ and fw_, which use it
    of::Translations translations_;
    of::MemoryMap virt_free_;
    of::MmuDriver mmu_;
    of::MemoryDriver memdrv_;
    u32 ram_window_ = 0;              // size of the RAM BAT window(s)
    of::Rtas rtas_;
    of::NvramDriver nvram_driver_;
    u64 rtc_base_ = 0;                // seconds at machine time 0
    bool powered_off_ = false, reboot_ = false;
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
