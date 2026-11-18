// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/bw_firmware.cpp — see bw_firmware.h.

#include "core/machine/bw_firmware.h"

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
    return l;
}

BwFirmware::BwFirmware(YosemiteMachine& m, const Config& c)
    : m_(m), layout_(Layout::for_ram(m.ram_size())),
      forth_(m.bus(), layout_.forth_data, layout_.forth_size),
      words_(forth_, tree_, inst_, layout_.scratch, layout_.scratch_size, c.load_base, layout_.base),
      fw_(tree_, m.bus()),
      disk_{of::DiskDriver(c.disk[0] ? *c.disk[0] : no_disk_), of::DiskDriver(c.disk[1] ? *c.disk[1] : no_disk_)}
{
    if (!m.ok() || m.ram_size() < kMinRam || c.load_base >= layout_.base)
        return;

    // PCI resources, as firmware assigns them at power-on.
    m.bridge().config_write(0x18, (1u << 16) | (1u << 8) | 0u, 0x7);  // subordinate 1, secondary 1, primary 0
    Paddington& mio = m.macio();
    mio.config_write(0x10, c.macio_base, 0xF);
    mio.config_write(0x04, 0x0006, 0x3);       // memory space + bus master
    if (mio.mapped_base() != c.macio_base)
        return;

    // The device tree.
    of::BwMachine desc;
    desc.ram_size = m.ram_size();
    desc.cpu_hz = static_cast<u32>(m.cpu_hz());
    desc.bus_hz = static_cast<u32>(m.bus_hz());
    desc.pvr = m.cpu().state().spr[ppc::spr::PVR];
    desc.macio_base = c.macio_base;
    if (!of::build_bw_device_tree(tree_, desc, nodes_))
        return;

    // Free memory: everything below the firmware region.
    memory_.add_free(0, layout_.base);

    // Devices.
    for (u32 i = 0; i < 2; ++i)
        if (c.disk[i])
            inst_.bind(nodes_.disk[i], &disk_[i]);
    inst_.bind(nodes_.console, &console_);
    const of::Ihandle con = inst_.open(nodes_.console, "");
    tree_.set_int(nodes_.chosen, "stdout", con);
    tree_.set_int(nodes_.chosen, "stdin", con);

    // Client interface and Forth words.
    if (!fw_.install(layout_.ci_entry))
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

} // namespace g3
