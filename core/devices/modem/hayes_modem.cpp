// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/modem/hayes_modem.cpp — see hayes_modem.h.

#include "core/devices/modem/hayes_modem.h"

namespace g3 {

namespace {
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
} // namespace

void HayesModem::reset()
{
    static const u8 kDefaults[kSRegs] = {0, 0, 43, 13, 10, 8, 2, 50, 2, 6, 14, 95, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5};
    for (u32 i = 0; i < kSRegs; ++i)
        s_[i] = kDefaults[i];
    echo_ = 1;
    verbose_ = 1;
    quiet_ = 0;
    speaker_ = 1;
    volume_ = 1;
    results_ = 4;
    amp_c_ = 1;
    amp_d_ = 2;
    line_n_ = 0;
    saw_a_ = false;
    pluses_ = 0;
}

// --- output to the Mac ------------------------------------------------------------

void HayesModem::put_byte(u8 b)
{
    if (q_n_ == kQueue)
        return;  // full: lost (the Mac is not reading)
    q_[(q_head_ + q_n_) % kQueue] = b;
    ++q_n_;
}

void HayesModem::put(const char* s)
{
    while (*s)
        put_byte(static_cast<u8>(*s++));
}

u32 HayesModem::to_mac(u8* out, u32 max)
{
    u32 n = 0;
    while (n < max && q_n_) {
        out[n++] = q_[q_head_];
        q_head_ = (q_head_ + 1) % kQueue;
        --q_n_;
    }
    return n;
}

void HayesModem::result(Result r)
{
    if (quiet_)
        return;
    if (!verbose_) {
        char buf[4] = {static_cast<char>('0' + r), '\r', 0, 0};
        put(buf);
        return;
    }
    const char* text = "OK";
    switch (r) {
    case kOk: text = "OK"; break;
    case kConnect: text = results_ ? "CONNECT 53333" : "CONNECT"; break;
    case kRing: text = "RING"; break;
    case kNoCarrier: text = "NO CARRIER"; break;
    case kError: text = "ERROR"; break;
    case kNoDialtone: text = "NO DIALTONE"; break;
    case kBusy: text = "BUSY"; break;
    case kNoAnswer: text = "NO ANSWER"; break;
    }
    put("\r\n");
    put(text);
    put("\r\n");
}

void HayesModem::emit(Event e, char digit)
{
    if (event_)
        event_(event_ctx_, e, digit);
}

// --- the Mac's side ---------------------------------------------------------------

void HayesModem::from_mac(u8 b, u64 now_us)
{
    switch (state_) {
    case State::Online: {
        const u64 guard = static_cast<u64>(s_[12]) * 20000u;  // 1/50 s units
        if (guard && b == s_[2]) {
            if (pluses_ == 0)
                pluses_ = now_us - last_rx_ >= guard ? 1 : 0;
            else if (pluses_ < 3 && now_us - plus_at_ < guard)
                ++pluses_;
            else
                pluses_ = 0;
            plus_at_ = now_us;
        } else {
            pluses_ = 0;
        }
        last_rx_ = now_us;
        if (line_data_)
            line_data_(line_ctx_, &b, 1);
        return;
    }
    case State::Dialing:
        hang_up(now_us, true);  // any character aborts the call
        return;
    case State::Command:
    case State::OnlineCommand:
        command_char(b, now_us);
        return;
    }
}

void HayesModem::command_char(u8 b, u64 now_us)
{
    const char c = static_cast<char>(b & 0x7F);
    if (echo_)
        put_byte(b);
    if (b == s_[3]) {
        execute(line_, line_n_, now_us);
        line_n_ = 0;
        return;
    }
    if (b == s_[4])
        return;
    if (b == s_[5]) {
        if (line_n_)
            --line_n_;
        return;
    }
    if (c == '/' && line_n_ == 1 && upper(line_[0]) == 'A') {  // "A/": the last line again
        line_n_ = 0;
        if (echo_)
            put("\r");
        execute(last_, last_n_, now_us);
        return;
    }
    if (line_n_ < kLine)
        line_[line_n_++] = c;
}

void HayesModem::execute(const char* line, u32 len, u64 now_us)
{
    u32 i = 0;
    while (i < len && line[i] == ' ')
        ++i;
    if (len - i < 2 || upper(line[i]) != 'A' || upper(line[i + 1]) != 'T')
        return;  // not a command line: modems say nothing
    if (line != last_) {
        last_n_ = len;
        for (u32 k = 0; k < len; ++k)
            last_[k] = line[k];
    }
    i += 2;
    auto number = [&](u32& value) {
        value = 0;
        bool any = false;
        while (i < len && is_digit(line[i])) {
            value = value * 10 + static_cast<u32>(line[i++] - '0');
            any = true;
        }
        return any;
    };
    while (i < len) {
        const char c = upper(line[i++]);
        u32 n = 0;
        switch (c) {
        case ' ':
            break;
        case 'E': case 'V': case 'Q': {
            number(n);
            if (n > 1) {
                result(kError);
                return;
            }
            (c == 'E' ? echo_ : c == 'V' ? verbose_ : quiet_) = static_cast<u8>(n);
            break;
        }
        case 'M': case 'L': case 'X': {
            number(n);
            if (n > (c == 'X' ? 4u : 3u)) {
                result(kError);
                return;
            }
            (c == 'M' ? speaker_ : c == 'L' ? volume_ : results_) = static_cast<u8>(n);
            break;
        }
        case 'H':
            number(n);
            if (state_ != State::Command)
                hang_up(now_us, false);
            break;
        case 'Z':
            number(n);
            if (state_ != State::Command)
                hang_up(now_us, false);
            reset();
            result(kOk);
            return;  // the rest of the line is ignored
        case 'I':
            number(n);
            if (n == 0)
                put("\r\n56000\r\n");
            else if (n == 3 || n == 4)
                put("\r\nG3MacPi virtual modem\r\n");
            break;
        case 'O':
            number(n);
            if (state_ != State::OnlineCommand) {
                result(kError);
                return;
            }
            go_online(now_us);
            return;
        case 'A':
            if (state_ != State::Command) {
                result(kError);
                return;
            }
            result(kNoCarrier);  // nobody is calling
            return;
        case 'D': {
            if (state_ != State::Command) {
                result(kError);
                return;
            }
            u32 used = 0;
            dial(line + i, len - i, used, now_us);
            return;  // the dial string is the rest of the line
        }
        case 'S': {
            number(n);
            if (n >= kSRegs) {
                result(kError);
                return;
            }
            if (i < len && line[i] == '=') {
                ++i;
                u32 v = 0;
                number(v);
                s_[n] = static_cast<u8>(v);
            } else if (i < len && line[i] == '?') {
                ++i;
                const u32 v = s_[n];
                char buf[8] = {'\r', '\n', static_cast<char>('0' + v / 100), static_cast<char>('0' + v / 10 % 10),
                               static_cast<char>('0' + v % 10), '\r', '\n', 0};
                put(buf);
            }
            break;
        }
        case '&': {
            const char k = i < len ? upper(line[i++]) : 0;
            number(n);
            if (k == 'F') {
                const State keep = state_;
                reset();
                state_ = keep;
            } else if (k == 'C') {
                amp_c_ = n ? 1 : 0;
            } else if (k == 'D') {
                amp_d_ = static_cast<u8>(n > 3 ? 2 : n);
            }
            break;  // &K, &W, &V ...: accepted
        }
        case '\\': case '%':
            if (i < len)
                ++i;
            number(n);
            break;
        case '+':
            while (i < len && line[i] != ';')
                ++i;
            break;
        case ';':
            break;
        default:
            if ((c >= 'A' && c <= 'Z') || is_digit(c)) {
                number(n);  // B, N, W, Y ...: accepted and ignored
                break;
            }
            result(kError);
            return;
        }
    }
    result(kOk);
}

bool HayesModem::dial(const char* p, u32 len, u32& used, u64 now_us)
{
    number_n_ = 0;
    pulse_ = false;
    stay_in_command_ = false;
    for (used = 0; used < len; ++used) {
        const char c = upper(p[used]);
        if (c == 'T')
            pulse_ = false;
        else if (c == 'P')
            pulse_ = true;
        else if (c == ';')
            stay_in_command_ = true;
        else if (c == 'L') {
            for (u32 k = 0; k < last_number_n_ && number_n_ < kNumber; ++k)
                number_[number_n_++] = last_number_[k];
        } else if ((is_digit(c) || c == '*' || c == '#' || c == ',' || (c >= 'A' && c <= 'D')) && number_n_ < kNumber)
            number_[number_n_++] = c;
        // W, spaces, dashes, brackets, '!', '@': nothing to do here
    }
    if (number_n_ == 0 && !stay_in_command_) {
        result(kNoCarrier);  // nobody to call
        return false;
    }
    last_number_n_ = number_n_;
    for (u32 k = 0; k < number_n_; ++k)
        last_number_[k] = number_[k];
    state_ = State::Dialing;
    step_ = Step::DialTone;
    emit(Event::OffHook);
    step_end_ = now_us + static_cast<u64>(timing_.dial_tone_ms) * 1000u;
    return true;
}

void HayesModem::next_dial_step(u64 now_us)
{
    auto start_digit = [&]() {
        const char d = number_[digit_];
        in_gap_ = false;
        u32 ms;
        if (d == ',') {
            emit(Event::Pause);
            ms = static_cast<u32>(s_[8]) * 1000u;
            in_gap_ = true;  // no gap after a pause
        } else if (pulse_) {
            const u32 pulses = is_digit(d) ? (d == '0' ? 10u : static_cast<u32>(d - '0')) : 1u;
            emit(Event::Digit, d);
            ms = pulses * timing_.pulse_ms;
        } else {
            emit(Event::Digit, d);
            ms = s_[11];
        }
        step_end_ += static_cast<u64>(ms) * 1000u;
    };
    switch (step_) {
    case Step::DialTone:
        step_ = Step::Digits;
        digit_ = 0;
        if (number_n_) {
            start_digit();
            return;
        }
        [[fallthrough]];
    case Step::Digits:
        if (number_n_ && !in_gap_) {
            in_gap_ = true;
            emit(Event::Pause);
            step_end_ += static_cast<u64>(pulse_ ? timing_.interdigit_ms : s_[11]) * 1000u;
            return;
        }
        if (number_n_ && ++digit_ < number_n_) {
            start_digit();
            return;
        }
        if (stay_in_command_) {  // ';': dialled, back to commands, still off hook
            state_ = State::Command;
            result(kOk);
            return;
        }
        step_ = Step::Ring;
        emit(Event::Ringback);
        step_end_ += static_cast<u64>(timing_.ring_ms) * 1000u;
        return;
    case Step::Ring:
        step_ = Step::Handshake;
        emit(Event::Handshake);
        step_end_ += static_cast<u64>(timing_.handshake_ms) * 1000u;
        return;
    case Step::Handshake:
        go_online(now_us);
        if (line_state_)
            line_state_(line_ctx_, true);
        return;
    }
}

void HayesModem::go_online(u64 now_us)
{
    state_ = State::Online;
    pluses_ = 0;
    last_rx_ = now_us;
    emit(Event::Connected);
    result(kConnect);
}

void HayesModem::hang_up(u64 now_us, bool report)
{
    const bool was_up = state_ == State::Online || state_ == State::OnlineCommand;
    state_ = State::Command;
    pluses_ = 0;
    (void)now_us;
    emit(Event::OnHook);
    if (was_up && line_state_)
        line_state_(line_ctx_, false);
    if (report)
        result(kNoCarrier);
}

void HayesModem::set_dtr(bool on, u64 now_us)
{
    if (dtr_ && !on) {  // acted on once it has stayed off for S25 (tick)
        dtr_off_at_ = now_us;
        dtr_pending_ = true;
    }
    if (on)
        dtr_pending_ = false;  // back on in time: ignored
    dtr_ = on;
    if (dtr_pending_ && s_[25] == 0)
        dtr_dropped(now_us);
}

void HayesModem::dtr_dropped(u64 now_us)
{
    dtr_pending_ = false;
    switch (amp_d_) {
    case 1:
        if (state_ == State::Online) {
            state_ = State::OnlineCommand;
            result(kOk);
        }
        break;
    case 2:
        if (state_ != State::Command)
            hang_up(now_us, false);
        break;
    case 3:
        if (state_ != State::Command)
            hang_up(now_us, false);
        reset();
        break;
    default:
        break;
    }
}

// --- the line's side --------------------------------------------------------------

void HayesModem::from_line(const u8* data, u32 len)
{
    if (state_ != State::Online)
        return;  // in command mode the Mac would see results mixed with data: dropped
    for (u32 i = 0; i < len; ++i)
        put_byte(data[i]);
}

void HayesModem::remote_hang_up(u64 now_us)
{
    if (state_ != State::Command)
        hang_up(now_us, true);
}

void HayesModem::tick(u64 now_us)
{
    if (dtr_pending_ && now_us - dtr_off_at_ >= static_cast<u64>(s_[25]) * 10000u)
        dtr_dropped(now_us);
    while (state_ == State::Dialing && now_us >= step_end_)
        next_dial_step(now_us);
    if (state_ == State::Online && pluses_ == 3) {
        const u64 guard = static_cast<u64>(s_[12]) * 20000u;
        if (now_us - plus_at_ >= guard) {
            state_ = State::OnlineCommand;
            pluses_ = 0;
            result(kOk);
        }
    }
}

} // namespace g3
