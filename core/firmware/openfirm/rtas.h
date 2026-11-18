// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/rtas.h — Run-Time Abstraction Services (RTAS):
// the /rtas node, its instantiate-rtas method, and the RTAS calls
// themselves (handled by an emulator trap word at the instantiated entry).
//
// Facts (docs/firmware/rtas.md):
//  - CHRP binding rev 1.7 §5.2: /rtas is a child of /, has no reg or
//    ranges, and has "rtas-size" (bytes of real memory RAS needs) and
//    "rtas-version". There is one property per implemented function, named
//    after it, whose value (encode-int) is the token for rtas-call. Method
//    instantiate-rtas ( rtas-base-address -- rtas-call ) binds RTAS to that
//    physical address and returns its entry point.
//  - The call (as Linux makes it, arch/powerpc/kernel/rtas_entry.S, read
//    for facts): MSR IR/DR/EE off, r3 = the argument buffer's physical
//    address, r4 = the RTAS base, LR = return address. Buffer: token,
//    nargs, nret, then nargs argument cells, then nret result cells; the
//    first result is the status: 0 success, -1 hardware error, -3
//    parameter error (Linux rtas.h).
//  - Functions (shapes as Linux calls them):
//      get-time-of-day  ( -- status year month day hour min sec nsec )
//      set-time-of-day  ( year month day hour min sec nsec -- status )
//      nvram-fetch      ( index buffer length -- status actual )
//      nvram-store      ( index buffer length -- status actual )
//      display-character ( char -- status )
//      power-off        ( power-on-hi power-on-lo -- status )
//      system-reboot    ( -- status )
//      read-pci-config  ( config-addr size -- status value )
//      write-pci-config ( config-addr size value -- status )
//    config-addr = (reg & 0xF00) << 20 | bus << 16 | devfn << 8 | (reg & 0xFF)
//    (Linux rtas_config_addr; PAPR 7.3.4-1); size 1, 2 or 4, aligned.
// Which functions Apple's Blue & White firmware offers is to be verified
// against a real device tree; this set is what CHRP defines and Mac OS may use.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_RTAS_H
#define G3_CORE_FIRMWARE_OPENFIRM_RTAS_H

#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"
#include "core/firmware/openfirm/device_tree.h"
#include "core/firmware/openfirm/instances.h"

namespace g3::of {

class Rtas final : public PackageDriver {
public:
    static constexpr u32 kSize = 0x1000;      // "rtas-size": one page holds our trap word
    static constexpr u32 kVersion = 1;        // "rtas-version" (value: to verify against [3])
    static constexpr s32 kSuccess = 0, kHardwareError = -1, kParameterError = -3;

    enum Token : u32 {
        kGetTimeOfDay = 1, kSetTimeOfDay, kNvramFetch, kNvramStore, kDisplayCharacter, kPowerOff, kSystemReboot,
        kReadPciConfig, kWritePciConfig, kTokenCount,
    };

    // What RTAS reaches in the machine.
    struct Hooks {
        void* ctx = nullptr;
        u32 nvram_size = 0;
        u8 (*nvram_read)(void* ctx, u32 index) = nullptr;
        void (*nvram_write)(void* ctx, u32 index, u8 value) = nullptr;
        u64 (*seconds)(void* ctx) = nullptr;            // current time, seconds since 1970-01-01 UTC
        void (*set_seconds)(void* ctx, u64 s) = nullptr;
        void (*put_char)(void* ctx, char c) = nullptr;
        void (*power)(void* ctx, bool reboot) = nullptr; // power-off or system-reboot
        // PCI configuration space: one 32-bit register (reg aligned to 4;
        // byte k of the register is bits 8k..8k+7, PCI order).
        u32 (*pci_read)(void* ctx, u32 bus, u32 devfn, u32 reg) = nullptr;
        void (*pci_write)(void* ctx, u32 bus, u32 devfn, u32 reg, u32 value, u32 byte_enables) = nullptr;
    };

    explicit Rtas(const Hooks& h) : h_(h) {}

    // Creates /rtas under `root`, with its properties and tokens.
    Phandle add_node(DeviceTree& t, Phandle root);

    bool open(u32, const char*) override { return true; }
    // instantiate-rtas: writes `trap_word` at the base (and remembers it).
    bool method(u32 slot, const char* name, forth::Forth& f) override;
    void set_trap_word(u32 w) { trap_word_ = w; }
    u32 base() const { return base_; }  // 0 until instantiated

    // Executes the call whose argument buffer is at `args` (as the CPU sees it).
    // True if the buffer could be read; the status is in the first result.
    bool call(ppc::MemoryInterface& mem, u32 args);
    u32 calls() const { return calls_; }

    // Calendar conversion (proleptic Gregorian, UTC), exposed for tests.
    static void to_calendar(u64 seconds, u32& year, u32& month, u32& day, u32& hour, u32& min, u32& sec);
    static bool from_calendar(u32 year, u32 month, u32 day, u32 hour, u32 min, u32 sec, u64& seconds);

private:
    Hooks h_;
    u32 trap_word_ = 0;
    u32 base_ = 0;
    u32 calls_ = 0;
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_RTAS_H
