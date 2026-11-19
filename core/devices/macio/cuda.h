// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/macio/cuda.h — the Cuda microcontroller (ADB, real-time
// clock, parameter RAM, power control) as seen through the VIA.
//
// Protocol (docs/hardware/via-cuda.md; Linux via-cuda.c host side and QEMU
// cuda.c device side agree):
//   Port B bit 3 TREQ (Cuda -> host, active low), bit 4 TACK and bit 5 TIP
//   (host -> Cuda, active low). Bytes move through the VIA shift register;
//   ACR bit 4 (SR_OUT) gives the direction. Every byte step ends with the
//   Cuda raising the VIA shift-register interrupt.
//   Host -> Cuda: TIP asserted with the first byte in SR; each TACK toggle
//     delivers the next byte; TIP negated ends the packet.
//   Cuda -> host: Cuda asserts TREQ + interrupt ("attention"); the host
//     reads SR and asserts TIP; each TIP/TACK edge loads the next byte; TREQ
//     is negated together with the last byte; TIP negated ends it.
//   Idle sync: with TIP negated, TREQ follows TACK.
// Packets: type 0 ADB, 1 Cuda command, 2 error. Command replies are
//   CUDA_PACKET, 0, command, data... ; errors ERROR_PACKET, code, CUDA_PACKET,
//   command (code 2 unknown command, 5 bad parameters).
// ADB: commands go to the attached AdbBus. Replies: ADB_PACKET, flag, cmd,
//   data; flag 0x02 = no device answered (as the Linux driver interprets
//   it). Autopoll (when enabled) polls devices every 'rate' ms and sends
//   their data unsolicited as ADB_PACKET, 0x40, cmd, data.
// Collision: if the host starts sending (TIP with SR_OUT) while the Cuda has
//   data waiting, the host backs off (Linux checks TREQ); the Cuda ignores
//   that aborted transfer and raises its attention again afterwards.

#ifndef G3_CORE_DEVICES_MACIO_CUDA_H
#define G3_CORE_DEVICES_MACIO_CUDA_H

#include "core/common/types.h"
#include "core/devices/adb/adb.h"
#include "core/devices/macio/via6522.h"
#include "core/sched/scheduler.h"

namespace g3 {

class Cuda final : public ViaPeripheral {
public:
    // Port B bits.
    static constexpr u8 kTREQ = 0x08;
    static constexpr u8 kTACK = 0x10;
    static constexpr u8 kTIP = 0x20;
    // Packet types (Linux include/uapi/linux/adb.h).
    static constexpr u8 kAdbPacket = 0, kCudaPacket = 1, kErrorPacket = 2, kTimerPacket = 3;
    // Error codes in ERROR_PACKET replies (DingusPPC's Cuda model, after
    // Apple's Cuda ERS).
    static constexpr u8 kErrBadPacket = 1, kErrBadCommand = 2, kErrBadSize = 3, kErrBadParam = 4, kErrI2c = 5;
    // Cuda memory: PRAM appears at 0x100-0x1FF (READ/WRITE MCU MEMORY).
    static constexpr u32 kPramInMcu = 0x100;
    // Commands (Linux include/uapi/linux/cuda.h).
    enum Command : u8 {
        kWarmStart = 0x00, kAutopoll = 0x01, kGet6805Addr = 0x02, kGetTime = 0x03,
        kGetPram = 0x07, kSet6805Addr = 0x08, kSetTime = 0x09, kPowerDown = 0x0A,
        kPowerUpTime = 0x0B, kSetPram = 0x0C, kMsReset = 0x0D, kSendDfac = 0x0E,
        kResetSystem = 0x11, kSetIpl = 0x12, kFileServerFlag = 0x13, kSetAutoRate = 0x14,
        kGetAutoRate = 0x16, kSetDeviceList = 0x19, kGetDeviceList = 0x1A, kOneSecondMode = 0x1B,
        kSetPowerMessages = 0x21, kGetSetIic = 0x22, kToggleWakeup = 0x23, kTimerTickle = 0x24,
        kCombinedIic = 0x25, kOutPb0 = 0x26,
    };
    static constexpr u32 kPramSize = 256;  // to verify for the B&W's Cuda
    static constexpr u32 kMaxPacket = 64;

    using EventCallback = void (*)(void* context);

    Cuda(Via6522& via, Scheduler& scheduler);

    // Delay between a host action and the Cuda's shift-register interrupt.
    void set_response_delay_ps(u64 ps) { delay_ps_ = ps; }
    // Real-time clock, seconds since 1904-01-01 at the current guest time.
    void set_rtc(u32 seconds_since_1904);
    u32 rtc() const;
    u8* pram() { return pram_; }
    void set_power_callbacks(EventCallback power_down, EventCallback reset, void* context);
    void attach_adb(AdbBus* bus) { adb_ = bus; }
    bool autopoll_enabled() const { return autopoll_; }
    u8 one_second_mode() const { return one_sec_mode_; }
    // Diagnostics: called with each packet the host sent (to_host false)
    // and each packet queued for the host (to_host true).
    using PacketTrace = void (*)(void* context, const u8* bytes, u32 len, bool to_host);
    void set_packet_trace(PacketTrace cb, void* context) { trace_ = cb; trace_ctx_ = context; }

    // ViaPeripheral
    void via_port_b_changed(u8 value, u8 driven) override;
    void via_sr_written(u8) override {}
    void via_sr_read() override {}

private:
    enum class State { Idle, Receiving, Sending, Collision };

    void signal_byte();             // schedule the SR interrupt
    static void sr_event(void* ctx, u64 now);
    void set_treq(bool asserted);
    void process_packet();
    void reply_cuda(u8 command, const u8* data, u32 len);
    void reply_error(u8 code, u8 command);
    void queue_reply(const u8* bytes, u32 len);
    void load_next_out_byte();
    void schedule_autopoll();
    static void autopoll_event(void* ctx, u64 now);
    void schedule_second();
    static void second_event(void* ctx, u64 now);
    u8 mcu_byte(u32 addr) const;

    Via6522& via_;
    Scheduler& sched_;
    u64 delay_ps_ = 20000000;  // 20 us
    Scheduler::Handle pending_ = Scheduler::kInvalidHandle;

    State state_ = State::Idle;
    bool tip_ = false, tack_ = false;  // asserted?
    u8 in_[kMaxPacket] = {};
    u32 in_len_ = 0;
    u8 out_[kMaxPacket] = {};
    u32 out_len_ = 0, out_pos_ = 0;
    bool out_pending_ = false;
    bool open_ended_ = false;   // after out_, keep sending Cuda memory from mcu_addr_
    u32 mcu_addr_ = 0;

    u64 rtc_base_ps_ = 0;
    u32 rtc_base_seconds_ = 0;
    u8 pram_[kPramSize] = {};
    bool autopoll_ = false;
    u8 autopoll_rate_ms_ = 11;  // reset value: to verify
    u16 device_list_ = 0;
    AdbBus* adb_ = nullptr;
    Scheduler::Handle autopoll_event_ = Scheduler::kInvalidHandle;
    // One-second mode (0 = off): a packet every RTC second when idle.
    u8 one_sec_mode_ = 0;
    bool one_sec_first_ = true, one_sec_missed_ = false;
    u32 last_second_ = 0;
    Scheduler::Handle second_event_ = Scheduler::kInvalidHandle;
    EventCallback power_down_ = nullptr, reset_ = nullptr;
    void* power_ctx_ = nullptr;
    PacketTrace trace_ = nullptr;
    void* trace_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MACIO_CUDA_H
