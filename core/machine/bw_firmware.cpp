// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/bw_firmware.cpp — see bw_firmware.h.

#include "core/machine/bw_firmware.h"
#include "core/storage/apple_partition_map.h"

#include "core/machine/bw_nvram.h"

#include "core/common/cstring.h"

namespace g3 {

BwFirmware::Layout BwFirmware::Layout::for_ram(u32 ram)
{
    Layout l{};
    l.base = ram > kFirmwareSize ? ram - kFirmwareSize : 0;
    l.ci_entry = l.base;                       // one trap word
    l.forth_data = l.base + 0x1000;
    l.forth_size = 0x10000;
    l.scratch = l.base + 0x11000;
    l.scratch_size = 0x4000;
    l.stack_top = l.base + 0x40000;            // 0x2B000 bytes of client stack below
    // The page table maps every translation, RAM included, so it is sized
    // for the RAM: one 8-PTE group per 2 pages... the PowerPC manual's
    // guideline of 64 KB per 8 MB (RAM / 128), within SDR1's 64 KB-32 MB.
    l.htab_size = 0x10000;
    while (l.htab_size < ram / 128 && l.htab_size < (32u << 20))
        l.htab_size <<= 1;
    l.htab = l.base > l.htab_size ? (l.base - l.htab_size) & ~(l.htab_size - 1) : 0;
    return l;
}

BwFirmware::BwFirmware(YosemiteMachine& m, const Config& c)
    : m_(m), layout_(Layout::for_ram(m.ram_size())),
      view_(m.bus(), m.cpu()),
      mmu_(tree_, translations_, virt_free_),
      memdrv_(memory_),
      rtas_(rtas_hooks(this)),
      nvram_driver_(MacioNvram::kSize,
                    [](void* ctx, u32 i) { return static_cast<BwFirmware*>(ctx)->m_.macio().nvram().byte(i); },
                    [](void* ctx, u32 i, u8 v) { static_cast<BwFirmware*>(ctx)->m_.macio().nvram().set_byte(i, v); }, this),
      forth_(view_, layout_.forth_data, layout_.forth_size),
      words_(forth_, tree_, inst_, layout_.scratch, layout_.scratch_size, c.load_base, layout_.base),
      fw_(tree_, view_),
      disk_{of::DiskDriver(c.disk[0] ? *c.disk[0] : no_disk_), of::DiskDriver(c.disk[1] ? *c.disk[1] : no_disk_)},
      display_(m.display())
{
    if (!m.ok() || m.ram_size() < kMinRam || m.ram_size() > kMaxRam || c.load_base >= layout_.base ||
        layout_.htab < kVectorsEnd + (1u << 20))
        return;

    // PCI resources, as firmware assigns them at power-on.
    m.bridge().config_write(0x18, (1u << 16) | (1u << 8) | 0u, 0x7);  // subordinate 1, secondary 1, primary 0
    // Forwarding windows (PCI-to-PCI Bridge Architecture 3.2.5): I/O base and
    // limit in 4 KB units (0x1C/0x1D), memory base and limit in 1 MB units
    // (0x20/0x22); the device tree's bridge "ranges" describe the same.
    const of::BridgeWindows w = of::bridge_windows(c.macio_base);
    m.bridge().config_write(0x1C, (((w.io_base + w.io_size - 1) >> 8) & 0xF0) << 8 | ((w.io_base >> 8) & 0xF0), 0x3);
    m.bridge().config_write(0x20, (((w.mem_base + w.mem_size - 1) >> 16) & 0xFFF0) << 16 | ((w.mem_base >> 16) & 0xFFF0),
                            0xF);
    m.bridge().config_write(0x04, 0x0007, 0x3);  // I/O, memory, bus master
    Paddington& mio = m.macio();
    mio.config_write(0x10, c.macio_base, 0xF);
    mio.config_write(0x04, 0x0006, 0x3);       // memory space + bus master
    if (mio.mapped_base() != c.macio_base)
        return;
    // The CMD646: I/O BARs at the addresses a real B&W's firmware gives it
    // (Linux log: command block 0x1440, control 0x1430, bus master 0x1400;
    // the secondary channel's are ours), I/O decoding and bus mastering on.
    static constexpr u32 kCmd646Io[5] = {0x1440, 0x1430, 0x1448, 0x1434, 0x1400};
    Cmd646& ide = m.cmd646();
    for (u32 i = 0; i < 5; ++i)
        ide.config_write(0x10 + 4 * i, kCmd646Io[i], 0xF);
    ide.config_write(0x3C, YosemiteMachine::kCmd646Irq, 0x1);  // interrupt line (for software)
    ide.config_write(0x04, 0x0005, 0x3);
    if (ide.io_base(0) == 0)
        return;

    // The device tree.
    of::BwMachine desc;
    desc.ram_size = m.ram_size();
    desc.cpu_hz = static_cast<u32>(m.cpu_hz());
    desc.bus_hz = static_cast<u32>(m.bus_hz());
    desc.pvr = m.cpu().state().spr[ppc::spr::PVR];
    desc.macio_base = c.macio_base;
    for (u32 i = 0; i < 5; ++i)
        desc.cmd646_io[i] = kCmd646Io[i];
    if (m.has_display()) {
        Framebuffer& fb = m.display();
        if (!fb.set_mode(c.width, c.height, c.depth))
            return;
        display_.fill(0, 0, 0, fb.width(), fb.height());  // black (palette 0 = black at power-on)
        fb.set_enabled(true);
        desc.vram_base = fb.vram_base();
        desc.vram_size = fb.vram_size();
        desc.regs_base = YosemiteMachine::kDisplayRegs;
        desc.regs_size = Framebuffer::kRegsSize;
        desc.width = fb.width();
        desc.height = fb.height();
        desc.depth = fb.depth();
        desc.linebytes = fb.pitch();
    }
    if (!of::build_bw_device_tree(tree_, desc, nodes_))
        return;
    if (nodes_.display != of::kNone && c.display_driver && c.display_driver_size &&
        !tree_.set_property(nodes_.display, "driver,AAPL,MacOS,PowerPC", c.display_driver, c.display_driver_size))
        return;

    // Free memory: everything below the page table and firmware region, except the
    // exception vectors (0 - 0x3FFF; PowerPC binding §4.1: memory that can't
    // be used for general purposes isn't available). Aligned claims come
    // from the top.
    memory_.add_free(kVectorsEnd, layout_.htab - kVectorsEnd);
    memory_.set_top_down(true);

    // Virtual-Mode (docs/firmware/virtual-mode.md). Translations at power-on:
    // RAM, mac-io and the display, each 1:1. RAM (and the I/O windows) are
    // covered by BATs; other translations go into the hashed page table.
    // Virtual space for claims: above the RAM window, below the I/O space.
    ram_window_ = 0x20000;                      // BAT blocks: 128 KB .. 256 MB
    while (ram_window_ < m.ram_size() && ram_window_ < (256u << 20))
        ram_window_ <<= 1;
    if (m.ram_size() > (256u << 20))
        ram_window_ = 512u << 20;               // two 256 MB BATs
    virt_free_.add_free(ram_window_, 0x80000000u - ram_window_);
    bool mapped = translations_.map(0, 0, m.ram_size(), of::wimg::M | of::wimg::PP_RW) &&
                  translations_.map(c.macio_base, c.macio_base, Paddington::kBarSize, of::wimg::kIoDefault);
    if (m.has_display())
        mapped = mapped &&
                 translations_.map(m.display().vram_base(), m.display().vram_base(), m.display().vram_size(),
                                   of::wimg::kIoDefault) &&
                 translations_.map(YosemiteMachine::kDisplayRegs, YosemiteMachine::kDisplayRegs,
                                   Framebuffer::kRegsSize, of::wimg::kIoDefault);
    if (!mapped || !apply_mmu())
        return;
    mmu_.set_io_test([](void*, u32 pa) { return pa >= 0x80000000u; }, nullptr);
    mmu_.attach(nodes_.cpu, sync_mmu, this);
    inst_.bind(nodes_.cpu, &mmu_);
    inst_.bind(nodes_.memory, &memdrv_);
    const of::Ihandle mmu_ih = inst_.open(nodes_.cpu, "");
    const of::Ihandle mem_ih = inst_.open(nodes_.memory, "");
    if (!mmu_ih || !mem_ih)
        return;
    tree_.set_int(nodes_.chosen, "mmu", mmu_ih);
    tree_.set_int(nodes_.chosen, "memory", mem_ih);
    // The interrupt controller (Paddington's PIC node). NewWorld firmware
    // names it in /chosen (Linux skips "/chosen/interrupt-controller" when
    // looking for one). The value is a phandle: Apple's Mac OS ROM loader
    // passes it straight to getprop (seen in its client interface calls).
    tree_.set_int(nodes_.chosen, "interrupt-controller", nodes_.pic);
    // NVRAM: formatted if blank or damaged, as firmware does (bw_nvram.h).
    if (!bw_nvram::valid(m.macio().nvram()))
        bw_nvram::format(m.macio().nvram());
    // /chosen "nvram" is an open instance of mac-io's nvram node
    // (Apple's Mac OS ROM loader looks for it; without it, it guesses an offset).
    inst_.bind(nodes_.nvram, &nvram_driver_);
    const of::Ihandle nv_ih = inst_.open(nodes_.nvram, "");
    if (!nv_ih)
        return;
    tree_.set_int(nodes_.chosen, "nvram", nv_ih);
    fw_.set_virtual_mode(true);

    // RTAS (docs/firmware/rtas.md).
    rtc_base_ = c.rtc_seconds;
    const of::Phandle rtas_node = rtas_.add_node(tree_, tree_.root());
    if (rtas_node == of::kNone)
        return;
    rtas_.set_trap_word(of::Firmware::kTrapRtas);
    inst_.bind(rtas_node, &rtas_);
    fw_.set_rtas(&rtas_);

    // Devices.
    for (u32 i = 0; i < 2; ++i)
        if (c.disk[i])
            inst_.bind(nodes_.disk[i], &disk_[i]);
    inst_.bind(nodes_.console, &console_);
    if (nodes_.display != of::kNone)
        inst_.bind(nodes_.display, &display_);
    of::Ihandle con = inst_.open(nodes_.console, "");
    if (nodes_.display != of::kNone) {
        // The screen is stdout; its text is also echoed to the console sink.
        display_.set_echo([](void* ctx, const char* t, u32 len) {
            static_cast<of::ConsoleDriver*>(ctx)->write(0, reinterpret_cast<const u8*>(t), len);
        }, &console_);
        const of::Ihandle scr = inst_.open(nodes_.display, "");
        if (scr)
            con = scr;
    }
    tree_.set_int(nodes_.chosen, "stdout", con);
    tree_.set_int(nodes_.chosen, "stdin", con);
    stdout_ = con;
    // Forth output (." type . cr ...) goes to stdout, as on real firmware.
    forth_.set_output([](void* ctx, const char* t, u32 len) {
        auto* self = static_cast<BwFirmware*>(ctx);
        if (of::PackageDriver* d = self->inst_.driver(self->stdout_))
            d->write(self->inst_.slot(self->stdout_), reinterpret_cast<const u8*>(t), len);
    }, this);

    // Client interface and Forth words.
    if (!fw_.install(layout_.ci_entry) || !fw_.install_vectors())
        return;
    fw_.attach(m.cpu());
    of::ClientInterface& ci = fw_.client();
    ci.set_memory(&memory_);
    ci.set_instances(&inst_);
    ci.set_forth(&forth_);
    ci.set_clock(clock_ms, this);
    words_.set_memory(&memory_);
    words_.set_go(go, this);
    words_.set_clock(clock_ms, this);
    ok_ = tree_.ok();
}

bool BwFirmware::in_bat_window(u32 va) const
{
    if (va < ram_window_)
        return true;
    if (va >= 0x80000000u && va < 0x90000000u)
        return true;                            // PCI / mac-io
    return m_.has_display() && va >= 0x90000000u && va < 0x92000000u;
}

bool BwFirmware::sync_mmu(void* ctx) { return static_cast<BwFirmware*>(ctx)->apply_mmu(); }

// BATs (PEM 7.4): BATU = BEPI | BL << 2 | Vs; BATL = BRPN | WIMG | PP.
// Page table: every translated page outside the BAT windows. Segment
// register n holds VSID n (Ks = Kp = 0: PP 00-10 read/write).
bool BwFirmware::apply_mmu()
{
    of::PageTable pt(m_.bus(), layout_.htab, layout_.htab_size);
    if (!pt.clear())
        return false;
    // Every translation gets PTEs, also inside the BAT windows: a client may
    // reprogram the BATs and rely on the page table for the firmware's
    // translations (Mac OS does: docs/firmware/virtual-mode.md).
    for (u32 i = 0; i < translations_.count(); ++i) {
        const of::Translation& t = translations_[i];
        for (u64 off = 0; off < t.size; off += of::Translations::kPage) {
            const u32 va = static_cast<u32>(t.virt + off), pa = static_cast<u32>(t.phys + off);
            if (in_bat_window(va) && va != pa)
                return false;                   // a BAT window is 1:1; it would hide this mapping
            if (!pt.insert(va >> 28, va, pa, t.mode))
                return false;                   // both PTE groups full
        }
    }
    ppc::CpuState& s = m_.cpu().state();
    for (u32 n = 0; n < 16; ++n)
        s.sr[n] = n;
    s.spr[ppc::spr::SDR1] = pt.sdr1();
    auto no_bat = [&](u32 index) {
        for (u32 k = 0; k < 2; ++k)
            s.spr[ppc::spr::DBAT0U + 2 * index + k] = s.spr[ppc::spr::IBAT0U + 2 * index + k] = 0;
    };
    auto bat = [&](u32 index, u32 base, u32 size, u32 wimg, bool code) {
        const u32 bl = (size >> 17) - 1;
        const u32 upper = (base & 0xFFFE0000u) | (bl << 2) | ppc::mmu::BATU_VS;
        const u32 lower = (base & 0xFFFE0000u) | (wimg & 0x78) | of::wimg::PP_RW;
        s.spr[ppc::spr::DBAT0U + 2 * index] = upper;
        s.spr[ppc::spr::DBAT0U + 2 * index + 1] = lower;
        s.spr[ppc::spr::IBAT0U + 2 * index] = code ? upper : 0;
        s.spr[ppc::spr::IBAT0U + 2 * index + 1] = code ? lower : 0;
    };
    const u32 first = ram_window_ > (256u << 20) ? (256u << 20) : ram_window_;
    bat(0, 0, first, of::wimg::M, true);
    if (ram_window_ > (256u << 20))
        bat(1, 256u << 20, 256u << 20, of::wimg::M, true);
    else
        no_bat(1);
    bat(2, 0x80000000u, 256u << 20, of::wimg::I | of::wimg::G, false);
    if (m_.has_display())
        bat(3, 0x90000000u, 32u << 20, of::wimg::I | of::wimg::G, false);
    else
        no_bat(3);
    s.msr |= ppc::msr::IR | ppc::msr::DR;
    m_.cpu().flush_tlb();
    return true;
}

of::Rtas::Hooks BwFirmware::rtas_hooks(BwFirmware* self)
{
    of::Rtas::Hooks h;
    h.ctx = self;
    h.nvram_size = MacioNvram::kSize;
    h.nvram_read = [](void* c, u32 i) { return static_cast<BwFirmware*>(c)->m_.macio().nvram().byte(i); };
    h.nvram_write = [](void* c, u32 i, u8 v) { static_cast<BwFirmware*>(c)->m_.macio().nvram().set_byte(i, v); };
    h.seconds = [](void* c) {
        auto* f = static_cast<BwFirmware*>(c);
        return static_cast<u64>(f->rtc_base_ + f->m_.scheduler().now() / 1000000000000ull);  // ps -> s
    };
    h.set_seconds = [](void* c, u64 s) {
        auto* f = static_cast<BwFirmware*>(c);
        f->rtc_base_ = s - f->m_.scheduler().now() / 1000000000000ull;
    };
    h.put_char = [](void* c, char ch) {
        auto* f = static_cast<BwFirmware*>(c);
        if (of::PackageDriver* d = f->inst_.driver(f->stdout_))
            d->write(f->inst_.slot(f->stdout_), reinterpret_cast<const u8*>(&ch), 1);
    };
    // Configuration cycles go through Grackle's bus 0 (and the bridge to bus 1),
    // as the CPU's own configuration accesses do.
    h.pci_read = [](void* c, u32 bus, u32 devfn, u32 reg) {
        return static_cast<BwFirmware*>(c)->m_.grackle().pci_bus().config_read_routed(0, bus, devfn, reg);
    };
    h.pci_write = [](void* c, u32 bus, u32 devfn, u32 reg, u32 v, u32 be) {
        static_cast<BwFirmware*>(c)->m_.grackle().pci_bus().config_write_routed(0, bus, devfn, reg, v, be);
    };
    h.power = [](void* c, bool reboot) {
        auto* f = static_cast<BwFirmware*>(c);
        (reboot ? f->reboot_ : f->powered_off_) = true;
        f->fw_.park_after_trap();
    };
    return h;
}

u32 BwFirmware::clock_ms(void* ctx)
{
    auto* self = static_cast<BwFirmware*>(ctx);
    return static_cast<u32>(self->m_.scheduler().now() / 1000000000ull);  // ps -> ms
}

void BwFirmware::go(void* ctx, u32 entry)
{
    auto* self = static_cast<BwFirmware*>(ctx);
    self->fw_.start_client(self->m_.cpu().state(), entry, self->layout_.stack_top);
    self->started_ = true;
    self->entry_ = entry;
}

bool BwFirmware::command(const char* text)
{
    if (!ok_)
        return false;
    return forth_.interpret(text, cstr::length(text));
}

bool BwFirmware::set_boot_device(u32 drive, BlockDevice& image, char* out, u32 cap)
{
    const of::Phandle node = drive == 0 ? nodes_.disk[0] : drive == 2 ? nodes_.cmd646_disk : of::kNone;
    if (node == of::kNone)
        return false;
    ApplePartitionMap map;
    if (!map.read(image))
        return false;
    u32 part = map.find_type("Apple_HFS");
    if (!part)
        part = map.find_type("Apple_HFSX");
    if (!part)
        return false;
    char path[256];
    u32 n = tree_.path_of(node, path, sizeof path);
    if (n == 0 || n + 20 >= sizeof path)
        return false;
    path[n++] = ':';
    char digits[12];
    u32 k = 0;
    do {
        digits[k++] = static_cast<char>('0' + part % 10);
        part /= 10;
    } while (part);
    while (k)
        path[n++] = digits[--k];
    const char tail[] = ",\\\\:tbxi";
    for (u32 i = 0; tail[i]; ++i)
        path[n++] = tail[i];
    path[n] = 0;
    if (out && cap) {
        u32 i = 0;
        for (; path[i] && i + 1 < cap; ++i)
            out[i] = path[i];
        out[i] = 0;
    }
    return tree_.set_string(nodes_.chosen, "bootpath", path);
}

} // namespace g3
