// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/modem/hayes_modem.h — a virtual dial-up modem that speaks
// the Hayes "AT" command set, for the Mac's serial port (docs/devices/
// modem.md). It does not modulate anything: once "connected", the bytes the
// Mac sends go to a line handler (the PPP "provider", ppp_server.h) and its
// replies come back. What it reproduces is the behaviour Mac OS's modem
// scripts and Remote Access see: command echo and result codes, the dial
// sequence and its timing (with events for the sound generator), carrier
// detect, the "+++" escape and DTR hang-up.
//
// Conventions followed (the Hayes command set as most modems of the
// period implemented it; the values are the common factory defaults):
//  - A command line starts with "AT" (either case), ends with S3 (CR);
//    S5 (BS) deletes; "A/" repeats the last line at once.
//  - E echo, V verbose results, Q quiet, M speaker mode (0 off, 1 until
//    connected, 2 always), L speaker volume (0-3), X result set (0: plain
//    CONNECT), H hang up, O back on line, Z reset, I information, A answer
//    (no call: NO CARRIER), Sn=v / Sn?, D dial; &F factory settings, &C
//    (0: DCD always on, 1: DCD follows the carrier), &D (0: DTR ignored,
//    1: drop to command mode, 2: hang up, 3: reset). Other commands
//    (&K, \N, %C, +MS ...) are accepted and ignored, so vendor init strings
//    get OK.
//  - Result codes, verbose (V1, "\r\n<text>\r\n") or numeric (V0,
//    "<n>\r"): OK 0, CONNECT 1, RING 2, NO CARRIER 3, ERROR 4,
//    NO DIALTONE 6, BUSY 7, NO ANSWER 8; CONNECT <speed> when X > 0.
//  - S registers: S0 0, S2 43 ('+', escape character), S3 13, S4 10, S5 8,
//    S6 2, S7 50 (seconds to wait for carrier), S8 2 (comma pause, s),
//    S11 95 (DTMF tone length, ms), S12 50 (escape guard time, 1/50 s),
//    S25 5 (how long DTR must stay off before &D acts, 1/100 s; 0: at
//    once). Without that delay Mac OS 9's PPP hung the modem up just after
//    CONNECT: re-opening the port resets the SCC, which drops DTR for a few
//    microseconds, and Apple's script sets &D3.
//    S0-S255 can be set and read: modems kept vendor settings in the high
//    numbers, and init strings set them (Apple's "PowerMac G3 Internal
//    56K" script sends S95=1); those have no effect here.
//  - Dial string: digits, * # A-D; T/P tone or pulse; W and spaces
//    ignored; ',' pauses S8 seconds; ';' returns to command mode after
//    dialling; L redials the last number. A character from the Mac while
//    dialling aborts the call (NO CARRIER).
//  - Escape: S12 of silence, three S2 characters (each within S12 of the
//    last), S12 of silence: command mode while staying connected (OK). The
//    three characters are also passed on as data, as modems do.

#ifndef G3_CORE_DEVICES_MODEM_HAYES_MODEM_H
#define G3_CORE_DEVICES_MODEM_HAYES_MODEM_H

#include "core/common/types.h"

namespace g3 {

class HayesModem {
public:
    // What the modem is doing, for the sound generator (each lasts until the
    // next event). Digit carries the digit; it is followed by a gap of the
    // same length before the next one.
    enum class Event : u8 { OffHook, Digit, Pause, Ringback, Handshake, Connected, OnHook };
    using EventFn = void (*)(void* context, Event e, char digit);
    using LineDataFn = void (*)(void* context, const u8* data, u32 len);  // on line: Mac -> provider
    using LineStateFn = void (*)(void* context, bool up);                // call up / down

    struct Timing {
        u32 dial_tone_ms = 1500;   // after going off hook
        u32 ring_ms = 4000;        // ringback before the answer
        u32 handshake_ms = 12000;  // answer tone and training
        u32 pulse_ms = 100;        // per pulse (10 per second)
        u32 interdigit_ms = 700;   // between pulse digits
    };
    static constexpr u32 kConnectSpeed = 53333;  // what 56K modems typically reported
    static constexpr u32 kQueue = 8192;          // bytes waiting for the Mac

    HayesModem() { reset(); }
    void set_events(EventFn fn, void* ctx) { event_ = fn; event_ctx_ = ctx; }
    void set_line(LineDataFn data, LineStateFn state, void* ctx)
    {
        line_data_ = data;
        line_state_ = state;
        line_ctx_ = ctx;
    }
    void set_timing(const Timing& t) { timing_ = t; }

    void reset();                         // power on / ATZ: factory settings, on hook

    // The Mac's side (through the serial port).
    void from_mac(u8 byte, u64 now_us);
    void set_dtr(bool on, u64 now_us);
    u32 to_mac(u8* out, u32 max);         // takes up to max waiting bytes
    u32 waiting_for_mac() const { return q_n_; }
    bool dcd() const { return amp_c_ == 0 || state_ == State::Online || state_ == State::OnlineCommand; }

    // The line's side.
    void from_line(const u8* data, u32 len);  // on line: provider -> Mac
    void remote_hang_up(u64 now_us);           // the provider ended the call

    void tick(u64 now_us);                    // dialling and escape timing

    bool online() const { return state_ == State::Online; }
    bool connected() const { return state_ == State::Online || state_ == State::OnlineCommand; }
    u8 speaker_mode() const { return speaker_; }
    bool pulse_dialing() const { return pulse_; }
    u8 speaker_volume() const { return volume_; }
    u8 s(u32 n) const { return n < kSRegs ? s_[n] : 0; }

private:
    enum class State : u8 { Command, Dialing, Online, OnlineCommand };
    enum Result : u8 { kOk = 0, kConnect = 1, kRing = 2, kNoCarrier = 3, kError = 4, kNoDialtone = 6,
                       kBusy = 7, kNoAnswer = 8 };
    static constexpr u32 kSRegs = 256, kLine = 64, kNumber = 48;

    void command_char(u8 c, u64 now_us);
    void execute(const char* line, u32 len, u64 now_us);
    bool dial(const char* p, u32 len, u32& used, u64 now_us);
    void result(Result r);
    void put(const char* s);
    void put_byte(u8 b);
    void hang_up(u64 now_us, bool report);
    void dtr_dropped(u64 now_us);
    void go_online(u64 now_us);
    void emit(Event e, char digit = 0);
    void next_dial_step(u64 now_us);

    State state_ = State::Command;
    u8 s_[kSRegs] = {};
    u8 echo_ = 1, verbose_ = 1, quiet_ = 0, speaker_ = 1, volume_ = 1, results_ = 4, amp_c_ = 1, amp_d_ = 2;
    bool dtr_ = false;
    bool dtr_pending_ = false;  // DTR went off; acted on after S25
    u64 dtr_off_at_ = 0;

    char line_[kLine] = {};
    u32 line_n_ = 0;
    char last_[kLine] = {};
    u32 last_n_ = 0;
    bool saw_a_ = false;  // the previous character was 'A' (for "A/")

    // Dialling: the number and where the sequence is.
    char number_[kNumber] = {};
    u32 number_n_ = 0;
    char last_number_[kNumber] = {};
    u32 last_number_n_ = 0;
    bool pulse_ = false, stay_in_command_ = false;
    enum class Step : u8 { DialTone, Digits, Ring, Handshake } step_ = Step::DialTone;
    u32 digit_ = 0;
    bool in_gap_ = false;
    u64 step_end_ = 0;

    // Escape detection (on line).
    u64 last_rx_ = 0;
    u32 pluses_ = 0;
    u64 plus_at_ = 0;

    u8 q_[kQueue] = {};
    u32 q_head_ = 0, q_n_ = 0;

    Timing timing_;
    EventFn event_ = nullptr;
    void* event_ctx_ = nullptr;
    LineDataFn line_data_ = nullptr;
    LineStateFn line_state_ = nullptr;
    void* line_ctx_ = nullptr;
};

} // namespace g3

#endif // G3_CORE_DEVICES_MODEM_HAYES_MODEM_H
