// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/client_interface.h — the Open Firmware client
// interface: the services a client program (Mac OS's boot code) calls.
//
// Facts (docs/firmware/open-firmware.md; IEEE 1275-1994 §6.3 and the
// PowerPC binding):
//  - The client passes the address of an argument array of 32-bit
//    big-endian cells: [0] address of the service name (NUL-terminated),
//    [1] number of arguments N, [2] number of return values M, then N
//    arguments, then M return cells that the firmware fills in.
//  - The call itself returns 0 when the service exists, -1 when it
//    doesn't (the result is placed in r3 by the caller of call()).
//  - Failure values inside the return cells are -1 (0xFFFFFFFF).
//
// Services so far: test, peer, child, parent, getproplen, getprop,
// nextprop, setprop, finddevice, canon, package-to-path, milliseconds,
// exit, enter. Instances (open/close/read/write/seek, call-method),
// memory (claim/release) and interpret come in later steps.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_CLIENT_INTERFACE_H
#define G3_CORE_FIRMWARE_OPENFIRM_CLIENT_INTERFACE_H

#include "core/common/types.h"
#include "core/cpu/ppc750/memory_interface.h"
#include "core/firmware/openfirm/device_tree.h"

namespace g3::of {

class ClientInterface {
public:
    static constexpr u32 kFail = 0xFFFFFFFFu;
    static constexpr u32 kMaxArgs = 16;
    static constexpr u32 kMaxString = 256;

    using EventCallback = void (*)(void* context);
    using ClockCallback = u32 (*)(void* context);  // milliseconds since start

    // mem: the guest's memory (addresses as the client passes them).
    ClientInterface(DeviceTree& tree, ppc::MemoryInterface& mem) : tree_(tree), mem_(mem) {}

    void set_exit(EventCallback cb, void* ctx) { exit_cb_ = cb; exit_ctx_ = ctx; }
    void set_enter(EventCallback cb, void* ctx) { enter_cb_ = cb; enter_ctx_ = ctx; }
    void set_clock(ClockCallback cb, void* ctx) { clock_cb_ = cb; clock_ctx_ = ctx; }

    // Handles one client call; returns the value for r3 (0 or kFail).
    u32 call(u32 args_addr);

    // Diagnostics: the name of the last service called and how many calls
    // named an unknown service (to find what Mac OS needs next).
    const char* last_service() const { return service_; }
    u32 unknown_calls() const { return unknown_calls_; }

private:
    bool read_cell(u32 addr, u32& v) { return mem_.read32(addr, v); }
    bool read_string(u32 addr, char* out, u32 max);  // false if unterminated or unreadable
    bool write_bytes(u32 addr, const u8* data, u32 len);

    u32 arg(u32 i) const { return args_[i]; }
    void ret(u32 i, u32 v);

    bool dispatch();

    DeviceTree& tree_;
    ppc::MemoryInterface& mem_;
    EventCallback exit_cb_ = nullptr;
    void* exit_ctx_ = nullptr;
    EventCallback enter_cb_ = nullptr;
    void* enter_ctx_ = nullptr;
    ClockCallback clock_cb_ = nullptr;
    void* clock_ctx_ = nullptr;

    // Current call.
    u32 args_addr_ = 0;
    u32 nargs_ = 0, nrets_ = 0;
    u32 args_[kMaxArgs] = {};
    char service_[64] = {};
    u32 unknown_calls_ = 0;
    u8 setprop_buf_[4096] = {};
};

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_CLIENT_INTERFACE_H
