// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/devices/adb/adb.h — Apple Desktop Bus: bus, keyboard and mouse.
//
// Command byte (Linux include/uapi/linux/adb.h): address << 4 | code, with
//   0x00 SendReset (all devices), 0x01 Flush, 0x08|reg Listen, 0x0C|reg Talk.
// Default addresses: keyboard 2, mouse 3 (Linux ADB_KEYBOARD/ADB_MOUSE).
// Register 0 formats (QEMU adb-kbd.c/adb-mouse.c, Linux adbhid.c agree):
//   keyboard: two key bytes, bit 7 = released, 0xFF = no key;
//             the power key is sent as 0x7F 0x7F (release 0xFF 0xFF);
//   mouse:    byte 0 = button 1 up (bit 7) | Y delta (7-bit two's complement,
//             positive = down); byte 1 = button 2 up (bit 7) | X delta.
//   A device with nothing to report does not answer a Talk R0.
// Register 3: Talk returns {SRQ-enable | address, handler id}; Listen sets
//   the address from byte 0 bits 3-0 and, unless the handler byte is one of
//   the special codes 0x00/0xFD/0xFE/0xFF, switches to that handler if the
//   device supports it. Details/uncertainties: docs/hardware/adb.md.

#ifndef G3_CORE_DEVICES_ADB_ADB_H
#define G3_CORE_DEVICES_ADB_ADB_H

#include "core/common/types.h"

namespace g3 {

class AdbDevice {
public:
    AdbDevice(u8 default_address, u8 default_handler)
        : default_address_(default_address), default_handler_(default_handler)
    {
    }

    u8 address() const { return address_; }
    u8 handler() const { return handler_; }

    // Returns the number of bytes written to 'out' (max 8); 0 = no answer.
    u32 talk(u32 reg, u8* out);
    void listen(u32 reg, const u8* data, u32 len);
    void reset();
    virtual void flush() {}
    virtual bool has_data() const = 0;

protected:
    ~AdbDevice() = default;
    virtual u32 talk_register(u32 reg, u8* out) = 0;      // registers 0-2
    virtual void listen_register(u32, const u8*, u32) {}  // registers 0-2
    virtual bool supports_handler(u8 handler) const = 0;
    virtual void device_reset() {}

private:
    const u8 default_address_;
    const u8 default_handler_;
    u8 address_ = default_address_;
    u8 handler_ = default_handler_;
};

class AdbBus {
public:
    static constexpr u32 kMaxDevices = 8;

    bool attach(AdbDevice* d);

    // Executes one ADB command. For Talk: returns the reply length (0 = no
    // device answered / nothing to report). Other commands return 0.
    u32 command(u8 cmd, const u8* data, u32 len, u8* reply);

    // Autopoll support: the first device (by address order in 'mask', or
    // all addresses if mask == 0) with pending data, or nullptr.
    AdbDevice* device_with_data(u16 mask) const;

private:
    AdbDevice* at(u32 address) const;
    AdbDevice* devices_[kMaxDevices] = {};
    u32 count_ = 0;
};

class AdbKeyboard final : public AdbDevice {
public:
    static constexpr u8 kPowerKey = 0x7F;
    AdbKeyboard() : AdbDevice(2, 1) {}

    // Host input: an ADB key code (0x00-0x7F) went down or up.
    void key_event(u8 adb_code, bool pressed);
    bool has_data() const override { return count_ > 0; }
    void flush() override { count_ = 0; }

private:
    u32 talk_register(u32 reg, u8* out) override;
    bool supports_handler(u8 h) const override { return h == 1 || h == 2 || h == 3; }
    void device_reset() override { count_ = 0; }
    bool pop(u8& code);

    static constexpr u32 kQueue = 32;
    u8 queue_[kQueue] = {};
    u32 head_ = 0, count_ = 0;
};

class AdbMouse final : public AdbDevice {
public:
    AdbMouse() : AdbDevice(3, 2) {}

    // Host input: relative motion (positive = right / down) and buttons
    // (bit 0 = primary button pressed).
    void motion(s32 dx, s32 dy);
    void buttons(u32 pressed_mask);
    bool has_data() const override;
    void flush() override { dx_ = dy_ = 0; reported_buttons_ = buttons_; }

private:
    u32 talk_register(u32 reg, u8* out) override;
    bool supports_handler(u8 h) const override { return h == 1 || h == 2; }
    void device_reset() override { dx_ = dy_ = 0; buttons_ = reported_buttons_ = 0; }

    s32 dx_ = 0, dy_ = 0;
    u32 buttons_ = 0, reported_buttons_ = 0;
};

} // namespace g3

#endif // G3_CORE_DEVICES_ADB_ADB_H
