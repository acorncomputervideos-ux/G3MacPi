// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/mac_session.h — one run of the emulated Macintosh, built
// from the user's configuration: the machine, our Open Firmware, the drives,
// the Mac OS ROM file, keyboard and mouse, and the screen. Front ends (the
// Pi kernel, the Linux host runner) supply memory and files and call run().
//
// Start-up is what the probe established (docs/devlog): the Mac OS ROM file
// (a CHRP boot file) is placed at load-base as Open Firmware's `load`
// would, then `init-program` runs its boot script, which starts Mac OS.
//
// Uses `new` for the machine and firmware (large objects, one per run);
// otherwise freestanding.

#ifndef G3_PLATFORM_COMMON_MAC_SESSION_H
#define G3_PLATFORM_COMMON_MAC_SESSION_H

#include "core/common/types.h"
#include "core/devices/ata/atapi_cdrom.h"
#include "core/machine/machine_config.h"
#include "core/storage/block_device.h"

namespace g3 {

class YosemiteMachine;
class BwFirmware;
struct MachineState;

// What a front end provides.
class MacHost {
public:
    // The whole file at `path` (the Mac OS ROM file). The bytes must stay
    // valid until start() returns. nullptr if it cannot be read.
    virtual const u8* load_file(const char* path, u32& size) = 0;
    // A disk or CD image. nullptr if it cannot be opened.
    virtual BlockDevice* open_image(const char* path, bool read_only) = 0;
    virtual void close_image(BlockDevice* image) = 0;
    // Text the firmware prints on its console (optional).
    virtual void console(const char* text, u32 len) { (void)text; (void)len; }
    // Mac OS ejected the disc in CD-ROM drive `drive` (its image is closed).
    virtual void disc_ejected(u32 drive) { (void)drive; }

protected:
    ~MacHost() = default;
};

class MacSession {
public:
    enum class Error : u8 {
        None,
        BadRam,          // memory size outside what the firmware supports
        NoMachine,       // the machine could not be wired (e.g. no RAM)
        Image,           // a drive's image could not be opened (see detail())
        NoRom,           // the Mac OS ROM file could not be read
        RomTooBig,       // it does not fit at load-base
        Firmware,        // Open Firmware failed to power on
        BootFailed,      // the boot script failed or did not start a client
    };

    MacSession() = default;
    ~MacSession();
    MacSession(const MacSession&) = delete;
    MacSession& operator=(const MacSession&) = delete;

    struct Memory {
        u8* ram = nullptr;
        u32 ram_size = 0;           // must be config.ram_mb MB
        u8* vram = nullptr;         // video memory (8 MB is enough for 1600x1200x32)
        u32 vram_size = 0;
        // With config.jit: memory for the CPU's block cache (a few MB) and,
        // optional, executable memory for generated code (AArch64 only).
        u8* blocks = nullptr;
        u32 blocks_size = 0;
        u32* code = nullptr;
        u32 code_words = 0;
    };
    // Builds and starts the Mac. `driver`: our display driver (PEF), which
    // Mac OS needs to draw; `unix_time`: the real-time clock at power-on;
    // `saved`: NVRAM and PRAM from the last run (machine_state.h), or none.
    Error start(const MachineConfig& config, MacHost& host, const Memory& memory, const u8* driver,
                u32 driver_size, u64 unix_time, const MachineState* saved = nullptr);
    // What to keep for the next run: the clock now, NVRAM and PRAM. False
    // when no Mac is running.
    bool capture_state(MachineState& out) const;
    void stop();  // releases everything (closes the images)

    bool running() const { return fw_ != nullptr; }
    Error error() const { return error_; }
    const char* detail() const { return detail_; }  // e.g. the drive or file at fault
    static const char* error_text(Error e);

    // Emulates `picoseconds` of guest time.
    void run(u64 picoseconds);
    bool halted() const;              // the CPU checkstopped
    u64 instructions() const;         // executed so far (speed statistics)
    bool block_cache_on() const;      // the CPU uses its decoded-block cache
    bool jit_on() const;              // ... and generated code
    bool powered_off() const;         // Mac OS shut down (RTAS power-off)
    bool reboot_requested() const;    // Mac OS restarted (RTAS system-reboot)

    // Input: a USB HID keyboard usage (press/release) and mouse movement.
    void key(u8 hid_usage, bool pressed);
    void mouse(s32 dx, s32 dy, u32 buttons);  // buttons: bit 0 = button 1

    // CD-ROM drives: put in another disc (path) or eject (empty path).
    // False if `drive` is not a CD-ROM or the image cannot be opened.
    bool change_disc(u32 drive, const char* path);

    // Screen: the Mac's frame buffer as 0x00RRGGBB pixels.
    bool screen_on() const;
    u32 screen_width() const;
    u32 screen_height() const;
    void screen(u32* pixels, u32 pitch_pixels);

    YosemiteMachine* machine() { return m_; }   // for diagnostics and tests
    BwFirmware* firmware() { return fw_; }

private:
    Error fail(Error e, const char* detail = "");
    static void console_sink(void* ctx, const char* text, u32 len);
    struct EjectSlot { MacSession* session; u32 drive; };
    static void disc_ejected(void* ctx);

    YosemiteMachine* m_ = nullptr;
    BwFirmware* fw_ = nullptr;
    MacHost* host_ = nullptr;
    BlockDevice* image_[MachineConfig::kDrives] = {};
    AtapiCdrom cd_[MachineConfig::kDrives];
    bool is_cd_[MachineConfig::kDrives] = {};
    EjectSlot eject_slot_[MachineConfig::kDrives] = {};
    Error error_ = Error::None;
    char detail_[MachineConfig::kPathMax] = {};
};

} // namespace g3

#endif // G3_PLATFORM_COMMON_MAC_SESSION_H
