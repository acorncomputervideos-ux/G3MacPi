// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/common/mac_session.cpp — see mac_session.h.

#include "platform/common/mac_session.h"
#include "platform/common/jit_report.h"
#include "platform/common/rom_from_disc.h"

#include "core/machine/bw_firmware.h"
#include "core/machine/yosemite.h"
#include "platform/common/hid_to_adb.h"
#include "platform/common/machine_state.h"
#include "platform/common/screen_presenter.h"

namespace g3 {

namespace {

// Where the boot file claims to come from, as Open Firmware's `load` records
// it (the boot script reads it back). The Mac OS ROM file normally lives in
// the System Folder of the start-up volume.
constexpr const char* kBootPath = "hd:2,\\\\:tbxi";

u32 text_len(const char* s)
{
    u32 n = 0;
    while (s[n])
        ++n;
    return n;
}

void copy_text(char* dst, u32 cap, const char* src)
{
    u32 i = 0;
    for (; src[i] && i + 1 < cap; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

} // namespace

MacSession::~MacSession() { stop(); }

const char* MacSession::error_text(Error e)
{
    switch (e) {
    case Error::None: return "no error";
    case Error::BadRam: return "memory size not supported";
    case Error::NoMachine: return "the machine could not be built";
    case Error::Image: return "a disk or CD image could not be opened";
    case Error::NoRom: return "the Mac OS ROM file could not be read";
    case Error::RomTooBig: return "the Mac OS ROM file is too large";
    case Error::Firmware: return "Open Firmware failed to start";
    case Error::BootFailed: return "the Mac OS ROM file did not start Mac OS";
    case Error::NotMacOsRom: return "no Mac OS ROM file to start from";
    }
    return "unknown error";
}

MacSession::Error MacSession::fail(Error e, const char* detail)
{
    copy_text(detail_, sizeof detail_, detail);  // first: `detail` may live in the firmware
    stop();
    error_ = e;
    return e;
}

void MacSession::disc_ejected(void* ctx)
{
    auto* slot = static_cast<EjectSlot*>(ctx);
    MacSession* self = slot->session;
    const u32 i = slot->drive;
    if (self->image_[i] && self->host_)
        self->host_->close_image(self->image_[i]);
    self->image_[i] = nullptr;
    if (self->host_)
        self->host_->disc_ejected(i);
}

void MacSession::stop()
{
    delete fw_;
    fw_ = nullptr;
    delete m_;
    m_ = nullptr;
    for (u32 i = 0; i < MachineConfig::kDrives; ++i) {
        cd_[i].insert(nullptr);
        if (image_[i] && host_)
            host_->close_image(image_[i]);
        image_[i] = nullptr;
        is_cd_[i] = false;
    }
}

void MacSession::console_sink(void* ctx, const char* text, u32 len)
{
    auto* self = static_cast<MacSession*>(ctx);
    if (self->host_)
        self->host_->console(text, len);
}

MacSession::Error MacSession::start(const MachineConfig& config, MacHost& host, const Memory& mem,
                                    const u8* driver, u32 driver_size, u64 unix_time, const MachineState* saved)
{
    stop();
    host_ = &host;
    error_ = Error::None;
    detail_[0] = 0;
    const u64 ram_bytes = static_cast<u64>(config.ram_mb) << 20;
    if (!mem.ram || mem.ram_size != ram_bytes || ram_bytes < BwFirmware::kMinRam || ram_bytes > BwFirmware::kMaxRam)
        return fail(Error::BadRam);

    YosemiteConfig yc;
    yc.ram = mem.ram;
    yc.ram_size = mem.ram_size;
    yc.vram = mem.vram;
    yc.vram_size = mem.vram_size;
    if (config.jit && mem.blocks) {
        yc.jit_memory = mem.blocks;
        yc.jit_size = mem.blocks_size;
        yc.jit_code = mem.code;
        yc.jit_code_words = mem.code_words;
        yc.jit_background = mem.jit_background;
        yc.jit_runner = mem.jit_runner;
        yc.jit_runner_context = mem.jit_runner_context;
    }
    yc.rtc_seconds_since_1904 = static_cast<u32>(unix_time + kUnixTo1904);
    m_ = new YosemiteMachine(yc);
    if (!m_ || !m_->ok())
        return fail(Error::NoMachine);
    // What the Mac kept from its last run: NVRAM (before the firmware, which
    // formats it only when blank or damaged) and the Cuda's PRAM.
    if (saved) {
        m_->macio().nvram().load(saved->nvram, MachineState::kNvramSize);
        m_->macio().nvram().clear_dirty();
        u8* pram = m_->macio().cuda().pram();
        for (u32 i = 0; i < MachineState::kPramSize && i < Cuda::kPramSize; ++i)
            pram[i] = saved->pram[i];
    }

    // Drives: ata0.master = mac-io ATA channel 0, device 0, and so on.
    BwFirmware::Config fc;
    for (u32 i = 0; i < MachineConfig::kDrives; ++i) {
        const MachineConfig::Drive& d = config.drive[i];
        if (d.type == MachineConfig::DriveType::None)
            continue;
        const bool cd = d.type == MachineConfig::DriveType::CdRom;
        if (d.image[0]) {
            image_[i] = host.open_image(d.image, cd || d.read_only);
            if (!image_[i])
                return fail(Error::Image, d.image);
        }
        // ata0: the mac-io ATA channel (the B&W's CD-ROM bus); ata1: the
        // CMD646's primary channel (its hard-disk bus).
        AtaBus& bus = i / 2 == 0 ? m_->macio().ide(0).bus() : m_->cmd646().channel(0);
        if (cd) {
            is_cd_[i] = true;
            eject_slot_[i] = EjectSlot{this, i};
            cd_[i].set_eject_callback(disc_ejected, &eject_slot_[i]);
            bus.attach_cdrom(i % 2, &cd_[i]);
            cd_[i].insert(image_[i]);  // nullptr: an empty drive
        } else if (image_[i]) {
            bus.attach(i % 2, image_[i]);
            if (i == 0)
                fc.disk[0] = image_[i];  // Open Firmware's "hd": the mac-io master disk
        }
    }

    fc.display_driver = driver;
    fc.display_driver_size = driver_size;
    fc.rtc_seconds = unix_time;
    fc.width = config.width;
    fc.height = config.height;
    fc.depth = config.depth == 16 ? 15 : config.depth;  // "thousands": the device's 15-bit mode
    // Sound and Ethernet: in the device tree when on, connected to the host.
    fc.sound = config.sound;
    fc.ethernet = config.network;
    host.mac_address(fc.mac_address);
    m_->macio().sound().set_sink(config.sound ? host.audio_sink() : nullptr);
    m_->macio().ethernet().set_link(config.network ? host.net_link() : nullptr);
    fw_ = new BwFirmware(*m_, fc);
    if (!fw_ || !fw_->ok())
        return fail(Error::Firmware);
    fw_->set_console(console_sink, this);

    // The Mac OS ROM file: the one in the settings if it is one (a CHRP boot
    // file), else the one in the System Folder of a CD in a drive (as a real
    // B&W's firmware finds it), read into the upper half of the Mac's RAM,
    // which nothing uses before the boot (the firmware sits at the top and
    // copies the file to its load address, far below).
    u32 rom_size = 0;
    const u8* rom = host.load_file(config.rom, rom_size);
    const bool file_ok = rom && rom_size != 0 && is_mac_os_rom(rom, rom_size);
    copy_text(rom_source_, sizeof rom_source_, config.rom);
    if (!file_ok) {
        const u32 cap = mem.ram_size / 4;
        u8* scratch = mem.ram + mem.ram_size / 2;
        for (u32 i = 0; i < MachineConfig::kDrives; ++i) {
            if (!is_cd_[i] || !image_[i])
                continue;
            const u32 n = find_mac_os_rom(*image_[i], scratch, cap);
            if (n) {
                rom = scratch;
                rom_size = n;
                copy_text(rom_source_, sizeof rom_source_, "the System Folder of the CD in the ");
                const u32 at = text_len(rom_source_);
                copy_text(rom_source_ + at, sizeof rom_source_ - at, MachineConfig::drive_label(i));
                break;
            }
        }
        if (rom != scratch) {
            // Say what is wrong with the file in the settings.
            if (!rom || rom_size == 0)
                return fail(Error::NoRom, config.rom);
            return fail(Error::NotMacOsRom,
                        rom_size >= (512u << 10) && rom_size <= (4u << 20)
                            ? "the ROM file is a ROM chip image of an older Mac. Use \"Mac OS ROM\" from "
                              "Mac OS 9's System Folder, or put a Mac OS 9 CD image in the CD drive."
                            : "the ROM file is not \"Mac OS ROM\" (from Mac OS 9's System Folder). Choose "
                              "that, or put a Mac OS 9 CD image in the CD drive.");
        }
    }
    // The start-up disk (boot = auto/cdrom/disk): /chosen bootpath, and the
    // same path for the loaded file. Without one the Mac shows the "?" disk
    // (or Mac OS 9.2 reports that it cannot find its bootpath).
    bool has_image[MachineConfig::kDrives];
    for (u32 i = 0; i < MachineConfig::kDrives; ++i)
        has_image[i] = image_[i] != nullptr;
    const u32 boot = choose_boot_drive(config, has_image);
    char boot_path[256];
    const bool booting = boot < MachineConfig::kDrives &&
                         fw_->set_boot_device(boot, *image_[boot], boot_path, sizeof boot_path);
    if (!fw_->place_file(rom, rom_size, booting ? boot_path : kBootPath))
        return fail(Error::RomTooBig, config.rom);
    if (!fw_->command("init-program") || !fw_->client_started())
        return fail(Error::BootFailed, fw_ ? fw_->forth().error_text() : "");
    return Error::None;
}

bool MacSession::capture_state(MachineState& out) const
{
    if (!m_)
        return false;
    out.rtc_1904 = m_->macio().cuda().rtc();
    const u8* nv = m_->macio().nvram().data();
    for (u32 i = 0; i < MachineState::kNvramSize; ++i)
        out.nvram[i] = nv[i];
    const u8* pram = m_->macio().cuda().pram();
    for (u32 i = 0; i < MachineState::kPramSize; ++i)
        out.pram[i] = i < Cuda::kPramSize ? pram[i] : 0;
    return true;
}

void MacSession::run(u64 picoseconds)
{
    if (m_ && fw_ && !halted())
        m_->run_for(picoseconds);
}

bool MacSession::halted() const { return m_ && m_->cpu().checkstopped(); }
bool MacSession::block_cache_on() const { return m_ && m_->cpu().block_cache() != nullptr; }
bool MacSession::jit_on() const { return m_ && m_->jit_enabled(); }

u32 MacSession::jit_report(char* out, u32 cap) const
{
    if (cap == 0)
        return 0;
    out[0] = 0;
    if (!jit_on())
        return 0;
    JitReport r;
    make_jit_report(m_->cpu(), r);
    return format_jit_report(r, out, cap);
}
ppc::JitWorker* MacSession::jit_worker() { return m_ ? m_->jit_worker() : nullptr; }

u64 MacSession::instructions() const { return m_ ? m_->cpu().instructions_executed() : 0; }
u64 MacSession::guest_time_ps() const { return m_ ? m_->loop().now_ps() : 0; }
void MacSession::set_effective_hz(u64 hz)
{
    if (m_)
        m_->loop().set_effective_hz(hz);
}
u64 MacSession::effective_hz() const { return m_ ? m_->loop().effective_hz() : 0; }
u64 MacSession::nominal_hz() const { return m_ ? m_->cpu_hz() : 0; }
bool MacSession::powered_off() const { return fw_ && fw_->powered_off(); }
bool MacSession::reboot_requested() const { return fw_ && fw_->reboot_requested(); }

void MacSession::key(u8 hid_usage, bool pressed)
{
    if (!m_)
        return;
    const u8 code = hid_usage_to_adb(hid_usage);
    if (code != kAdbNoKey)
        m_->keyboard().key_event(code, pressed);
}

void MacSession::mouse(s32 dx, s32 dy, u32 buttons)
{
    if (!m_)
        return;
    if (dx || dy)
        m_->mouse().motion(dx, dy);
    m_->mouse().buttons(buttons);
}

bool MacSession::change_disc(u32 drive, const char* path)
{
    if (!m_ || drive >= MachineConfig::kDrives || !is_cd_[drive])
        return false;
    BlockDevice* next = nullptr;
    if (path && path[0]) {
        next = host_->open_image(path, true);
        if (!next)
            return false;
    }
    cd_[drive].insert(next);
    if (image_[drive])
        host_->close_image(image_[drive]);
    image_[drive] = next;
    return true;
}

bool MacSession::screen_on() const { return m_ && m_->has_display() && m_->display().enabled(); }
u32 MacSession::screen_width() const { return m_ && m_->has_display() ? m_->display().width() : 0; }
u32 MacSession::screen_height() const { return m_ && m_->has_display() ? m_->display().height() : 0; }

void MacSession::screen(u32* pixels, u32 pitch_pixels)
{
    if (m_ && m_->has_display())
        m_->display().convert(pixels, pitch_pixels);
}

void MacSession::screen_frame(ScreenFrame& f) const
{
    f.on = false;
    if (!m_ || !m_->has_display())
        return;
    Framebuffer& fb = m_->display();
    f.on = fb.enabled();
    f.width = fb.width();
    f.height = fb.height();
    f.depth = fb.depth();
    f.pitch = fb.pitch();
    f.vram = fb.vram();
    f.vram_size = fb.vram_size();
    for (u32 i = 0; i < 256; ++i)
        f.palette[i] = fb.palette(i);
}

} // namespace g3
