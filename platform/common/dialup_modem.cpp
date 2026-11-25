// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/dialup_modem.cpp — see dialup_modem.h.

#include "platform/common/dialup_modem.h"

namespace g3 {

namespace {
constexpr u64 kPsPerUs = 1000000u;
constexpr u32 kDefaultBaud = 57600;
constexpr u64 kIdlePs = 1000u * kPsPerUs;  // 1 ms when nothing is waiting for the Mac
} // namespace

DialupModem::DialupModem(MacioEscc& escc, Scheduler& sched, const u8 mac_address[6])
    : escc_(escc), sched_(sched), link_(mac_address)
{
    escc_.set_tx_sink([](void* c, u32 ch, u8 b) {
        auto* d = static_cast<DialupModem*>(c);
        if (ch == kChannel)
            d->modem_.from_mac(b, d->now_us());
    }, this);
    escc_.set_lines_callback([](void* c, u32 ch, bool dtr, bool) {
        auto* d = static_cast<DialupModem*>(c);
        if (ch == kChannel)
            d->modem_.set_dtr(dtr, d->now_us());
    }, this);
    escc_.set_cts(kChannel, true);
    modem_.set_line([](void* c, const u8* data, u32 n) {
        auto* d = static_cast<DialupModem*>(c);
        for (u32 i = 0; i < n; ++i)
            d->ppp_.receive(data[i], d->now_us());
    }, [](void* c, bool up) {
        auto* d = static_cast<DialupModem*>(c);
        if (up) {
            d->ppp_.start(d->now_us());
            d->link_.start(d->now_us());
        } else {
            d->ppp_.stop();
            d->link_.stop();
        }
    }, this);
    modem_.set_events([](void* c, HayesModem::Event e, char digit) {
        auto* d = static_cast<DialupModem*>(c);
        if (d->sound_)
            d->sound_->post(e, digit, d->modem_.pulse_dialing(), d->modem_.speaker_mode(), d->modem_.speaker_volume());
    }, this);
    ppp_.set_output([](void* c, const u8* data, u32 n) { static_cast<DialupModem*>(c)->modem_.from_line(data, n); },
                    this);
    ppp_.set_ip_receiver([](void* c, const u8* p, u32 n) {
        auto* d = static_cast<DialupModem*>(c);
        d->link_.from_mac(p, n, d->now_us());
    }, this);
    ppp_.set_hang_up([](void* c) {
        auto* d = static_cast<DialupModem*>(c);
        d->modem_.remote_hang_up(d->now_us());
    }, this);
    link_.set_mac_side([](void* c, const u8* p, u32 n) { static_cast<DialupModem*>(c)->ppp_.send_ip(p, n); }, this);
    link_.set_ready([](void* c, const PppServer::Addresses& a) {
        auto* d = static_cast<DialupModem*>(c);
        d->ppp_.set_addresses(a, d->now_us());
    }, this);
    schedule_next(sched_.now());
}

DialupModem::~DialupModem()
{
    if (scheduled_)
        sched_.cancel(event_);
    escc_.set_tx_sink(nullptr, nullptr);
    escc_.set_lines_callback(nullptr, nullptr);
    if (tee_)
        tee_->set_watcher(nullptr, nullptr);
}

void DialupModem::set_network(NetTee* tee)
{
    tee_ = tee;
    DialupLink::FrameFn send = [](void* c, const u8* f, u32 n) { static_cast<NetTee*>(c)->send(f, n); };
    link_.set_network(tee ? send : nullptr, tee);
    if (tee)
        tee->set_watcher([](void* c, const u8* f, u32 n) {
            auto* d = static_cast<DialupModem*>(c);
            d->link_.from_network(f, n, d->now_us());
        }, this);
}

void DialupModem::schedule_next(u64 now_ps)
{
    u64 delay = kIdlePs;
    if (modem_.waiting_for_mac()) {
        const u32 baud = escc_.baud(kChannel);
        delay = 10u * 1000000u * kPsPerUs / (baud ? baud : kDefaultBaud);  // one character time
    }
    event_ = sched_.schedule_at(now_ps + delay, &DialupModem::on_tick, this);
    scheduled_ = true;
}

void DialupModem::on_tick(void* ctx, u64 now_ps)
{
    auto* d = static_cast<DialupModem*>(ctx);
    d->scheduled_ = false;
    const u64 us = now_ps / kPsPerUs;
    if (d->tee_)
        d->tee_->pump();
    d->modem_.tick(us);
    d->ppp_.tick(us);
    d->link_.tick(us);
    if (d->modem_.waiting_for_mac() && d->escc_.rx_ready(kChannel)) {
        u8 b;
        if (d->modem_.to_mac(&b, 1))
            d->escc_.rx_push(kChannel, b);
    }
    d->escc_.set_dcd(kChannel, d->modem_.dcd());
    d->schedule_next(now_ps);
}

} // namespace g3
