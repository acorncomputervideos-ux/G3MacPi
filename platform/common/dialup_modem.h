// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/dialup_modem.h — the virtual dial-up modem put together
// (docs/devices/modem.md): the Hayes modem on the SCC's channel A, the PPP
// provider behind it, and its network side on the Mac's network link.
//
//  - The Mac's bytes on channel A go to the modem; its DTR to the modem's
//    DTR. The modem's output goes into the SCC's receiver one character
//    per character time at the port's baud rate (10 bits a character),
//    as it would arrive; the modem drives DCD, and CTS is always on.
//  - On line, the modem's data goes to the PPP end; IP from it goes to the
//    network side and back. The call ends when the Mac's PPP terminates.
//  - The speaker: the modem's events go to a ModemSound, if there is one.
//  - Time: the machine's scheduler (guest time).
//
// Freestanding; the emulation core.

#ifndef G3_PLATFORM_COMMON_DIALUP_MODEM_H
#define G3_PLATFORM_COMMON_DIALUP_MODEM_H

#include "core/devices/macio/escc.h"
#include "core/devices/modem/hayes_modem.h"
#include "core/sched/scheduler.h"
#include "platform/common/dialup_link.h"
#include "platform/common/modem_sound.h"
#include "platform/common/net_tee.h"
#include "platform/common/ppp_server.h"

namespace g3 {

class DialupModem {
public:
    static constexpr u32 kChannel = MacioEscc::A;  // the traditional modem port (docs/hardware/escc.md)

    DialupModem(MacioEscc& escc, Scheduler& sched, const u8 mac_address[6]);
    ~DialupModem();
    void set_network(NetTee* tee);   // nullptr: no network (calls connect, DHCP never answers)
    void set_sound(ModemSound* s) { sound_ = s; }

    HayesModem& modem() { return modem_; }
    const PppServer& ppp() const { return ppp_; }
    const DialupLink& link() const { return link_; }

private:
    static void on_tick(void* ctx, u64 now_ps);
    void schedule_next(u64 now_ps);
    u64 now_us() const { return sched_.now() / 1000000u; }

    MacioEscc& escc_;
    Scheduler& sched_;
    HayesModem modem_;
    PppServer ppp_;
    DialupLink link_;
    NetTee* tee_ = nullptr;
    ModemSound* sound_ = nullptr;
    Scheduler::Handle event_ = 0;
    bool scheduled_ = false;
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_DIALUP_MODEM_H
