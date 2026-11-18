// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/bw_device_tree.cpp — see bw_device_tree.h and
// docs/firmware/bw-device-tree.md (sources of every value).

#include "core/firmware/openfirm/bw_device_tree.h"

namespace g3::of {

namespace {

// IEEE 1275 PCI binding: phys.hi = npt000ss bbbbbbbb dddddfff rrrrrrrr.
constexpr u32 pci_hi(u32 space, u32 bus, u32 dev, u32 fn, u32 reg)
{
    return (space << 24) | (bus << 16) | (dev << 11) | (fn << 8) | reg;
}
constexpr u32 kConfig = 0x00, kIo = 0x01, kMem32 = 0x02;

// Several NUL-terminated strings as one property.
bool set_strings(DeviceTree& t, Phandle h, const char* name, const char* const* s, u32 n)
{
    u8 buf[256];
    u32 len = 0;
    for (u32 i = 0; i < n; ++i) {
        for (const char* p = s[i]; *p; ++p) {
            if (len >= sizeof buf)
                return false;
            buf[len++] = static_cast<u8>(*p);
        }
        if (len >= sizeof buf)
            return false;
        buf[len++] = 0;
    }
    return t.set_property(h, name, buf, len);
}

void hex_unit(char* out, u32 v)
{
    char tmp[9];
    u32 n = 0;
    do {
        const u32 d = v & 15;
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    } while (v);
    for (u32 i = 0; i < n; ++i)
        out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

// A PCI function node: name, reg (configuration address), IDs.
Phandle pci_node(DeviceTree& t, Phandle parent, const char* name, u32 bus, u32 dev, u32 vendor, u32 device)
{
    char unit[9];
    hex_unit(unit, dev);
    const Phandle h = t.add_node(parent, name, unit);
    const u32 reg[5] = {pci_hi(kConfig, bus, dev, 0, 0), 0, 0, 0, 0};
    t.set_ints(h, "reg", reg, 5);
    t.set_int(h, "vendor-id", vendor);
    t.set_int(h, "device-id", device);
    return h;
}

// A mac-io cell: reg (offset/size pairs) and interrupts.
Phandle cell(DeviceTree& t, Phandle macio, const char* name, const char* type, const u32* reg, u32 nreg,
             const u32* irqs, u32 nirqs)
{
    char unit[9];
    hex_unit(unit, reg[0]);
    const Phandle h = t.add_node(macio, name, unit);
    if (type)
        t.set_string(h, "device_type", type);
    t.set_ints(h, "reg", reg, nreg);
    if (nirqs)
        t.set_ints(h, "interrupts", irqs, nirqs);
    return h;
}

} // namespace

bool build_bw_device_tree(DeviceTree& t, const BwMachine& m, BwNodes& n)
{
    n = BwNodes{};
    // --- root
    n.root = t.root();
    t.set_string(n.root, "name", "device-tree");
    t.set_string(n.root, "model", "PowerMac1,1");
    static const char* const compat[] = {"PowerMac1,1", "MacRISC", "Power Macintosh"};
    set_strings(t, n.root, "compatible", compat, 3);
    t.set_int(n.root, "#address-cells", 1);
    t.set_int(n.root, "#size-cells", 1);
    t.set_int(n.root, "clock-frequency", m.bus_hz);

    // --- cpus
    n.cpus = t.add_node(n.root, "cpus");
    t.set_int(n.cpus, "#address-cells", 1);
    t.set_int(n.cpus, "#size-cells", 0);
    n.cpu = t.add_node(n.cpus, "PowerPC,750", "0");
    t.set_string(n.cpu, "device_type", "cpu");
    t.set_int(n.cpu, "reg", 0);
    t.set_int(n.cpu, "cpu-version", m.pvr);
    t.set_int(n.cpu, "clock-frequency", m.cpu_hz);
    t.set_int(n.cpu, "bus-frequency", m.bus_hz);
    t.set_int(n.cpu, "timebase-frequency", m.bus_hz / 4);
    t.set_int(n.cpu, "d-cache-size", 32768);
    t.set_int(n.cpu, "i-cache-size", 32768);
    t.set_int(n.cpu, "d-cache-block-size", 32);
    t.set_int(n.cpu, "i-cache-block-size", 32);
    t.set_int(n.cpu, "d-cache-sets", 128);
    t.set_int(n.cpu, "i-cache-sets", 128);

    // --- memory
    n.memory = t.add_node(n.root, "memory", "0");
    t.set_string(n.memory, "device_type", "memory");
    const u32 mem_reg[2] = {0, m.ram_size};
    t.set_ints(n.memory, "reg", mem_reg, 2);

    // --- firmware nodes
    n.chosen = t.add_node(n.root, "chosen");
    n.aliases = t.add_node(n.root, "aliases");
    n.options = t.add_node(n.root, "options");
    n.openprom = t.add_node(n.root, "openprom");
    t.set_string(n.openprom, "model", "OpenFirmware 3");
    n.packages = t.add_node(n.root, "packages");
    n.console = t.add_node(n.packages, "terminal-emulator");

    // --- PCI host bridge (Grackle, MPC106)
    n.pci = t.add_node(n.root, "pci", "80000000");
    t.set_string(n.pci, "device_type", "pci");
    t.set_string(n.pci, "compatible", "grackle");
    t.set_int(n.pci, "#address-cells", 3);
    t.set_int(n.pci, "#size-cells", 2);
    const u32 pci_reg[2] = {0x80000000u, 0x7F000000u};
    t.set_ints(n.pci, "reg", pci_reg, 2);
    const u32 bus_range[2] = {0, 1};
    t.set_ints(n.pci, "bus-range", bus_range, 2);
    t.set_int(n.pci, "clock-frequency", 33333333);
    // ranges: PCI memory 1:1 from 0x80000000, PCI I/O at 0xFE000000 (to verify).
    const u32 pci_ranges[] = {
        pci_hi(kMem32, 0, 0, 0, 0), 0, 0x80000000u, 0x80000000u, 0, 0x7D000000u,
        pci_hi(kIo, 0, 0, 0, 0), 0, 0, 0xFE000000u, 0, 0x00C00000u,
    };
    t.set_ints(n.pci, "ranges", pci_ranges, sizeof pci_ranges / 4);

    // --- PCI-PCI bridge (DEC 21154) and the mac-io chip behind it
    n.bridge = pci_node(t, n.pci, "pci-bridge", 0, m.bridge_device, 0x1011, 0x0026);
    t.set_string(n.bridge, "device_type", "pci");
    t.set_int(n.bridge, "#address-cells", 3);
    t.set_int(n.bridge, "#size-cells", 2);
    const u32 bridge_range[2] = {1, 1};
    t.set_ints(n.bridge, "bus-range", bridge_range, 2);

    n.macio = pci_node(t, n.bridge, "mac-io", 1, m.macio_device, 0x106B, 0x0017);
    t.set_string(n.macio, "compatible", "paddington");
    const u32 bar = pci_hi(kMem32, 1, m.macio_device, 0, 0x10);
    const u32 assigned[5] = {bar | 0x80000000u, 0, m.macio_base, 0, 0x80000};  // n bit: not relocatable
    t.set_ints(n.macio, "assigned-addresses", assigned, 5);
    t.set_int(n.macio, "#address-cells", 1);
    t.set_int(n.macio, "#size-cells", 1);
    const u32 macio_ranges[5] = {0, bar, 0, m.macio_base, 0x80000};
    t.set_ints(n.macio, "ranges", macio_ranges, 5);
    t.set_property(n.macio, "interrupt-controller", nullptr, 0);
    t.set_int(n.macio, "#interrupt-cells", 1);

    // --- mac-io cells (offsets and interrupts as emulated: Paddington)
    const u32 cuda_reg[2] = {0x16000, 0x2000}, cuda_irq[1] = {18};
    n.cuda = cell(t, n.macio, "via-cuda", "via-cuda", cuda_reg, 2, cuda_irq, 1);
    const u32 ata_reg[2][4] = {{0x20000, 0x1000, 0x8B00, 0x100}, {0x21000, 0x1000, 0x8D00, 0x100}};
    const u32 ata_irq[2][2] = {{13, 2}, {14, 3}};
    for (u32 i = 0; i < 2; ++i) {
        n.ata[i] = cell(t, n.macio, "ata", "ata", ata_reg[i], 4, ata_irq[i], 2);
        t.set_string(n.ata[i], "compatible", "heathrow-ata");
        t.set_int(n.ata[i], "#address-cells", 1);
        t.set_int(n.ata[i], "#size-cells", 0);
        n.disk[i] = t.add_node(n.ata[i], "disk", "0");
        t.set_string(n.disk[i], "device_type", "block");
        t.set_int(n.disk[i], "reg", 0);
    }
    const u32 nvram_reg[2] = {0x60000, 0x20000};
    n.nvram = cell(t, n.macio, "nvram", "nvram", nvram_reg, 2, nullptr, 0);

    // --- aliases
    char path[256];
    t.path_of(n.disk[0], path, sizeof path);
    t.set_string(n.aliases, "hd", path);
    t.set_string(n.aliases, "ide0", path);
    t.path_of(n.disk[1], path, sizeof path);
    t.set_string(n.aliases, "ide1", path);
    t.path_of(n.macio, path, sizeof path);
    t.set_string(n.aliases, "mac-io", path);
    return t.ok();
}

} // namespace g3::of
