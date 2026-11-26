// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/adb/adb.cpp — see adb.h.

#include "core/devices/adb/adb.h"

namespace g3 {

// --- generic device ------------------------------------------------------------------

void AdbDevice::reset()
{
    address_ = default_address_;
    handler_ = default_handler_;
    device_reset();
}

u32 AdbDevice::talk(u32 reg, u8* out)
{
    if (reg == 3) {
        // Bit 13 (0x20 in the high byte) = service request enable; enabled
        // after reset per Apple's ADB documentation (to verify per device).
        out[0] = static_cast<u8>(0x20 | (address_ & 0x0F));
        out[1] = handler_;
        return 2;
    }
    return talk_register(reg, out);
}

void AdbDevice::listen(u32 reg, const u8* data, u32 len)
{
    if (reg != 3) {
        listen_register(reg, data, len);
        return;
    }
    if (len < 2)
        return;
    const u8 new_address = data[0] & 0x0F;
    const u8 code = data[1];
    if (code == 0xFF)
        return;  // self-test request: nothing to change
    // 0x00 / 0xFD / 0xFE: address change only. Collision detection (0xFE:
    // move only if no other device answered) is not needed while each
    // address has one device.
    if (new_address != 0)
        address_ = new_address;
    if (code != 0x00 && code != 0xFD && code != 0xFE && supports_handler(code))
        handler_ = code;
}

// --- bus ---------------------------------------------------------------------------------

bool AdbBus::attach(AdbDevice* d)
{
    if (!d || count_ >= kMaxDevices)
        return false;
    devices_[count_++] = d;
    return true;
}

AdbDevice* AdbBus::at(u32 address) const
{
    for (u32 i = 0; i < count_; ++i)
        if (devices_[i]->address() == address)
            return devices_[i];
    return nullptr;
}

u32 AdbBus::command(u8 cmd, const u8* data, u32 len, u8* reply)
{
    const u32 address = cmd >> 4;
    const u32 code = cmd & 0x0F;
    if (code == 0x00) {  // SendReset: every device
        for (u32 i = 0; i < count_; ++i)
            devices_[i]->reset();
        return 0;
    }
    AdbDevice* d = at(address);
    if (!d)
        return 0;
    if (code == 0x01) {
        d->flush();
        return 0;
    }
    if ((code & 0x0C) == 0x08) {
        d->listen(code & 3, data, len);
        return 0;
    }
    if ((code & 0x0C) == 0x0C)
        return d->talk(code & 3, reply);
    return 0;
}

AdbDevice* AdbBus::device_with_data(u16 mask) const
{
    for (u32 addr = 1; addr < 16; ++addr) {
        if (mask && !(mask & (1u << addr)))
            continue;
        AdbDevice* d = at(addr);
        if (d && d->has_data())
            return d;
    }
    return nullptr;
}

// --- keyboard ------------------------------------------------------------------------------

void AdbKeyboard::key_event(u8 code, bool pressed)
{
    if (count_ >= kQueue)
        return;  // overflow: drop (a real keyboard has a small buffer too)
    queue_[(head_ + count_++) % kQueue] = static_cast<u8>((code & 0x7F) | (pressed ? 0 : 0x80));
}

bool AdbKeyboard::pop(u8& code)
{
    if (count_ == 0)
        return false;
    code = queue_[head_];
    head_ = (head_ + 1) % kQueue;
    --count_;
    return true;
}

u32 AdbKeyboard::talk_register(u32 reg, u8* out)
{
    if (reg == 2) {
        // Modifier/LED state. Exact bit layout not yet implemented: all keys
        // up, LEDs off (active-low bits) — to verify.
        out[0] = 0xFF;
        out[1] = 0xFF;
        return 2;
    }
    if (reg != 0)
        return 0;
    u8 first;
    if (!pop(first))
        return 0;
    if ((first & 0x7F) == kPowerKey) {
        // The power key is reported as both bytes (0x7F 0x7F / 0xFF 0xFF).
        out[0] = first;
        out[1] = first;
        return 2;
    }
    out[0] = first;
    u8 second;
    if (count_ > 0 && (queue_[head_] & 0x7F) != kPowerKey && pop(second))
        out[1] = second;
    else
        out[1] = 0xFF;
    return 2;
}

// --- mouse ----------------------------------------------------------------------------------

void AdbMouse::motion(s32 dx, s32 dy)
{
    dx_ += dx;
    dy_ += dy;
}

void AdbMouse::buttons(u32 pressed_mask)
{
    buttons_ = pressed_mask & 3;
}

bool AdbMouse::has_data() const
{
    return dx_ != 0 || dy_ != 0 || buttons_ != reported_buttons_;
}

u32 AdbMouse::talk_register(u32 reg, u8* out)
{
    if (reg != 0 || !has_data())
        return 0;
    auto clamp = [](s32 v) { return v < -63 ? -63 : (v > 63 ? 63 : v); };
    const s32 x = clamp(dx_), y = clamp(dy_);
    dx_ -= x;
    dy_ -= y;
    reported_buttons_ = buttons_;
    out[0] = static_cast<u8>(((buttons_ & 1) ? 0x00 : 0x80) | (static_cast<u32>(y) & 0x7F));
    out[1] = static_cast<u8>(((buttons_ & 2) ? 0x00 : 0x80) | (static_cast<u32>(x) & 0x7F));
    return 2;
}

} // namespace g3
