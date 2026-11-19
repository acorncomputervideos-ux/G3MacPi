// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/bw_device_tree.cpp — see bw_device_tree.h and
// docs/firmware/bw-device-tree.md (sources of every value).

#include "core/firmware/openfirm/bw_device_tree.h"

#include "core/devices/macio/heathrow_pic.h"
#include "core/devices/macio/paddington.h"

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
Phandle cell(DeviceTree& t, Phandle macio, Phandle pic, const char* name, const char* type, const u32* reg,
             u32 nreg, const u32* irqs, u32 nirqs)
{
    char unit[9];
    hex_unit(unit, reg[0]);
    const Phandle h = t.add_node(macio, name, unit);
    if (type)
        t.set_string(h, "device_type", type);
    t.set_ints(h, "reg", reg, nreg);
    if (nirqs) {
        t.set_ints(h, "interrupts", irqs, nirqs);
        // IEEE 1275 interrupt binding: the controller these numbers belong
        // to. Apple's Mac OS ROM loader asks for it.
        t.set_int(h, "interrupt-parent", pic);
    }
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
    // Interoperability only: Apple's Mac OS ROM loader (MacOS.elf) refuses
    // to continue ("Official Apple copyright message missing.") unless the
    // root's "copyright" starts with "Copyright " and contains " Apple
    // Computer, Inc." — the two strings it holds. The notice states Apple's
    // copyright in their firmware design; ours isn't Apple's. Added with the
    // project owner's agreement (docs/firmware/virtual-mode.md, devlog).
    t.set_string(n.root, "copyright", kAppleCopyrightNotice);
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
    // PowerPC binding §5.1.4-5.1.5 properties, with the 750's values (750 UM:
    // lwarx/stwcx. reservations are per 32-byte cache block; separate
    // instruction and data TLBs of 128 entries each, two-way, so 64 sets).
    // Apple's Mac OS ROM loader requires reservation-granule-size.
    t.set_int(n.cpu, "reservation-granule-size", 32);
    t.set_property(n.cpu, "tlb-split", nullptr, 0);
    t.set_int(n.cpu, "tlb-size", 256);
    t.set_int(n.cpu, "tlb-sets", 64);
    t.set_int(n.cpu, "d-tlb-size", 128);
    t.set_int(n.cpu, "d-tlb-sets", 64);
    t.set_int(n.cpu, "i-tlb-size", 128);
    t.set_int(n.cpu, "i-tlb-sets", 64);
    t.set_property(n.cpu, "graphics", nullptr, 0);         // fres, frsqrte, fsel (750 UM)
    t.set_property(n.cpu, "performance-monitor", nullptr, 0);

    // --- memory
    n.memory = t.add_node(n.root, "memory", "0");
    t.set_string(n.memory, "device_type", "memory");
    const u32 mem_reg[2] = {0, m.ram_size};
    t.set_ints(n.memory, "reg", mem_reg, 2);

    // --- firmware nodes
    n.chosen = t.add_node(n.root, "chosen");
    n.aliases = t.add_node(n.root, "aliases");
    n.options = t.add_node(n.root, "options");
    // Virtual-Mode (PowerPC binding §4.2; docs/firmware/virtual-mode.md).
    t.set_string(n.options, "real-mode?", "false");
    // /rom: Apple's Mac OS ROM boot script creates /rom/macos under it.
    // Our node carries no properties yet (what Apple's has: to verify).
    n.rom = t.add_node(n.root, "rom");
    n.openprom = t.add_node(n.root, "openprom");
    t.set_string(n.openprom, "model", "OpenFirmware 3");
    n.packages = t.add_node(n.root, "packages");
    n.console = t.add_node(n.packages, "terminal-emulator");

    // --- PCI host bridge (Grackle, MPC106)
    n.pci = t.add_node(n.root, "pci", "80000000");
    t.set_string(n.pci, "device_type", "pci");
    t.set_string(n.pci, "compatible", "grackle");
    t.set_string(n.pci, "model", "MOT,MPC106");  // as OpenBIOS names Grackle; Mac OS's PCI code knows the model
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
    // The windows the bridge forwards (programmed by the firmware into its
    // I/O and memory base/limit registers), 1:1. Mac OS translates the
    // addresses of devices behind the bridge through these; without them
    // the CMD646's I/O addresses stayed bare port numbers and its driver
    // wrote to RAM (docs/hardware/cmd646.md).
    const BridgeWindows w = bridge_windows(m.macio_base);
    const u32 bridge_ranges[] = {
        pci_hi(kIo, 0, 0, 0, 0), 0, w.io_base, pci_hi(kIo, 0, 0, 0, 0), 0, w.io_base, 0, w.io_size,
        pci_hi(kMem32, 0, 0, 0, 0), 0, w.mem_base, pci_hi(kMem32, 0, 0, 0, 0), 0, w.mem_base, 0, w.mem_size,
    };
    t.set_ints(n.bridge, "ranges", bridge_ranges, sizeof bridge_ranges / 4);

    n.macio = pci_node(t, n.bridge, "mac-io", 1, m.macio_device, 0x106B, 0x0017);
    t.set_string(n.macio, "compatible", "paddington");
    // Mac OS's heathrow-ata driver finds the chip's base by searching for a
    // node with device_type "dbdma" or "mac-io" and reading its AAPL,address
    // (which Mac OS derives from assigned-addresses). Without it the driver
    // fails before touching the hardware (disassembly of the ROM driver,
    // docs/firmware/bw-device-tree.md).
    t.set_string(n.macio, "device_type", "mac-io");
    const u32 bar = pci_hi(kMem32, 1, m.macio_device, 0, 0x10);
    const u32 assigned[5] = {bar | 0x80000000u, 0, m.macio_base, 0, 0x80000};  // n bit: not relocatable
    t.set_ints(n.macio, "assigned-addresses", assigned, 5);
    t.set_int(n.macio, "#address-cells", 1);
    t.set_int(n.macio, "#size-cells", 1);
    const u32 macio_ranges[5] = {0, bar, 0, m.macio_base, 0x80000};
    t.set_ints(n.macio, "ranges", macio_ranges, 5);

    // The CMD646 hard-disk controller (a PCI function next to mac-io). A
    // real B&W boots its hard disk from /pci@80000000/pci-bridge@d/pci-ata@1/
    // ata-4@0/@0 (users' boot-device paths); Mac OS's ROM driver for it is
    // "cmd646-ata" and reads reg, AAPL,address (from assigned-addresses)
    // and the interrupt (driver-ist). docs/hardware/cmd646.md.
    if (m.cmd646_io[0]) {
        n.cmd646 = pci_node(t, n.bridge, "pci-ata", 1, m.cmd646_device, 0x1095, 0x0646);
        t.set_string(n.cmd646, "compatible", "cmd646-ata");
        t.set_string(n.cmd646, "device_type", "pci-ide");
        t.set_int(n.cmd646, "revision-id", 0x07);
        t.set_int(n.cmd646, "class-code", 0x01018F);
        static const u32 kSizes[5] = {8, 4, 8, 4, 16};
        u32 reg[5 * 6], ide_assigned[5 * 5];
        reg[0] = pci_hi(kConfig, 1, m.cmd646_device, 0, 0);
        reg[1] = reg[2] = reg[3] = reg[4] = 0;
        for (u32 i = 0; i < 5; ++i) {
            const u32 hi = pci_hi(kIo, 1, m.cmd646_device, 0, 0x10 + 4 * i);
            u32* r = reg + 5 + 5 * i;
            r[0] = hi; r[1] = 0; r[2] = 0; r[3] = 0; r[4] = kSizes[i];
            u32* a = ide_assigned + 5 * i;
            a[0] = hi | 0x80000000u; a[1] = 0; a[2] = m.cmd646_io[i]; a[3] = 0; a[4] = kSizes[i];
        }
        t.set_ints(n.cmd646, "reg", reg, 5 * 6);
        t.set_ints(n.cmd646, "assigned-addresses", ide_assigned, 5 * 5);
        t.set_int(n.cmd646, "#address-cells", 1);
        t.set_int(n.cmd646, "#size-cells", 0);
        n.cmd646_channel = t.add_node(n.cmd646, "ata-4", "0");
        t.set_string(n.cmd646_channel, "device_type", "ata");
        // Mac OS's ATA Manager binds the cmd646-ata driver to the channel.
        t.set_string(n.cmd646_channel, "compatible", "cmd646-ata");
        t.set_int(n.cmd646_channel, "reg", 0);
        t.set_int(n.cmd646_channel, "#address-cells", 1);
        t.set_int(n.cmd646_channel, "#size-cells", 0);
        n.cmd646_disk = t.add_node(n.cmd646_channel, "disk", "0");
        t.set_string(n.cmd646_disk, "device_type", "block");
        t.set_int(n.cmd646_disk, "reg", 0);
    } else {
        n.cmd646 = n.cmd646_channel = n.cmd646_disk = kNone;
    }

    // Paddington's interrupt controller as its own node, which is how Apple's
    // Mac OS ROM loader finds it: a node of device_type "interrupt-controller"
    // whose compatible is "heathrow" or "paddington" (the loader's table of
    // special nodes; it also looks up /pci/mac-io/interrupt-controller).
    // The loader takes its "Heathrow base address" from this reg, and Mac OS
    // then programs the IRQ 0-31 bank at base + 0x10..0x1F (enable +0x14,
    // clear +0x18) and IRQs 32-63 at base + 0x00..0x0F (seen in a trace of
    // its PIC writes). The banks are at mac-io + 0x20 and + 0x10 (Linux
    // pmac_pic.c, which uses the mac-io base): so reg starts at mac-io + 0x10
    // and spans 0x20 bytes. With reg at mac-io + 0 Mac OS enabled its
    // interrupts in the wrong bank and never got the VIA's.
    // docs/firmware/bw-device-tree.md.
    const u32 pic_reg[2] = {Paddington::kPicOffset + kPicNodeOffset, kPicNodeSize};
    n.pic = t.add_node(n.macio, "interrupt-controller", "10");
    t.set_string(n.pic, "device_type", "interrupt-controller");
    t.set_string(n.pic, "compatible", "paddington");
    t.set_ints(n.pic, "reg", pic_reg, 2);
    t.set_property(n.pic, "interrupt-controller", nullptr, 0);
    t.set_int(n.pic, "#interrupt-cells", 1);
    if (n.cmd646 != kNone) {
        // INTA of the CMD646 is Paddington input 26 (the interrupt Linux
        // reports for it on a B&W); named directly, as for the mac-io cells.
        t.set_int(n.cmd646, "interrupts", 26);
        t.set_int(n.cmd646, "interrupt-parent", n.pic);
    }

    // --- mac-io cells (offsets and interrupts as emulated: Paddington)
    const u32 cuda_reg[2] = {0x16000, 0x2000}, cuda_irq[1] = {18};
    n.cuda = cell(t, n.macio, n.pic, "via-cuda", "via-cuda", cuda_reg, 2, cuda_irq, 1);
    // compatible "cuda": the Mac OS ROM file's parcel for Cuda machines
    // matches on it (with device_type "via-cuda") and gives the node
    // pef,AAPL,MacOS,PowerPC,register and code,AAPL,MacOS,name, which is how
    // Mac OS finds its PowerMgrPlugin. NativePowerMgrLib imports that
    // library, so without it NativePowerMgrLib is never prepared and every
    // library that depends on it (the Time Manager among them) fails to
    // initialise. OpenBIOS gives this node the same compatible.
    // docs/firmware/bw-device-tree.md.
    t.set_string(n.cuda, "compatible", "cuda");
    // The ADB bus behind Cuda. Apple's Mac OS ROM loader only reports "Cuda
    // ADB detected" for a child named "adb" with compatible "adb" (seen with
    // AAPL,debug); without it the Mac OS ROM drives the VIA with the Egret
    // protocol (TIP/TACK active high, cf. Linux via-cuda.c mcu_is_egret) and
    // waits forever for the first byte. docs/firmware/bw-device-tree.md.
    n.adb = t.add_node(n.cuda, "adb", nullptr);
    t.set_string(n.adb, "device_type", "adb");
    t.set_string(n.adb, "compatible", "adb");
    const u32 ata_reg[2][4] = {{0x20000, 0x1000, 0x8B00, 0x100}, {0x21000, 0x1000, 0x8D00, 0x100}};
    const u32 ata_irq[2][2] = {{13, 2}, {14, 3}};
    for (u32 i = 0; i < 2; ++i) {
        n.ata[i] = cell(t, n.macio, n.pic, "ata", "ata", ata_reg[i], 4, ata_irq[i], 2);
        t.set_string(n.ata[i], "compatible", "heathrow-ata");
        // Read by Mac OS's heathrow-ata driver: it fails without at least five
        // AAPL,pio-timing words, and the ATA Manager reads AAPL,bus-id. Values
        // as OpenBIOS gives these nodes (to verify against a real B&W; the
        // emulated controller ignores timing).
        const u32 pio_timing[8] = {0x526, 0x85, 0x25, 0x25, 0x25, 0, 0, 0};
        t.set_ints(n.ata[i], "AAPL,pio-timing", pio_timing, 8);
        t.set_int(n.ata[i], "AAPL,bus-id", i);
        t.set_int(n.ata[i], "#address-cells", 1);
        t.set_int(n.ata[i], "#size-cells", 0);
        n.disk[i] = t.add_node(n.ata[i], "disk", "0");
        t.set_string(n.disk[i], "device_type", "block");
        t.set_int(n.disk[i], "reg", 0);
    }
    // MESH SCSI: a real Blue & White has one (Linux on PowerMac1,1: "mesh:
    // configured for synchronous 5 MB/s"; interrupts 12 and 0, DBDMA channel 0
    // at mac-io + 0x8000). Apple's Mac OS ROM loader recognises it by the
    // name "scsi" (its special-node table; with "mesh" it ignored the node),
    // and the 68K start-up waits forever without its interrupt vector. Linux's
    // MESH driver matches device_type "scsi" + compatible "chrp,mesh0".
    // Registers at mac-io + 0x10000. docs/firmware/bw-device-tree.md.
    const u32 scsi_reg[4] = {Paddington::kMeshOffset, 0x1000, 0x8000, 0x100}, scsi_irq[2] = {Paddington::kMeshIrq, 0};
    n.scsi = cell(t, n.macio, n.pic, "scsi", "scsi", scsi_reg, 4, scsi_irq, 2);
    t.set_string(n.scsi, "compatible", "chrp,mesh0");
    // ESCC serial (Z85C30): a real B&W has one (Linux: ttyPZ0 at mac-io +
    // 0x13020 on interrupt 15, ttyPZ1 at + 0x13000 on 16). Apple's Mac OS
    // ROM loader takes the SCC's base from "escc-legacy" and the channels'
    // interrupt vectors from the MacRISC "escc" children (its special-node
    // table: escc-legacy/chrp,es1, ch-a/chrp,es2, ch-b/chrp,es3 and
    // ch-a/chrp,es4, ch-b/chrp,es5, type "serial"). Without them the 68K
    // start-up waits forever on the SCC interrupt vector.
    {
        const u32 base = Paddington::kEsccOffset;
        const u32 legacy_reg[2] = {base, 0x1000};
        const Phandle legacy = t.add_node(n.macio, "escc-legacy", "12000");
        t.set_string(legacy, "device_type", "escc-legacy");
        t.set_string(legacy, "compatible", "chrp,es1");
        t.set_ints(legacy, "reg", legacy_reg, 2);
        t.set_int(legacy, "#address-cells", 1);   // children's reg: one address, one size cell
        t.set_int(legacy, "#size-cells", 1);
        const u32 la_reg[2] = {base + 2, 1}, lb_reg[2] = {base, 1};
        const u32 irq_a[1] = {Paddington::kEsccIrq[0]}, irq_b[1] = {Paddington::kEsccIrq[1]};
        t.set_string(cell(t, legacy, n.pic, "ch-a", "serial", la_reg, 2, irq_a, 1), "compatible", "chrp,es2");
        t.set_string(cell(t, legacy, n.pic, "ch-b", "serial", lb_reg, 2, irq_b, 1), "compatible", "chrp,es3");
        const u32 escc_reg[2] = {base + 0x1000, 0x1000};
        n.escc = t.add_node(n.macio, "escc", "13000");
        t.set_string(n.escc, "device_type", "escc");
        t.set_ints(n.escc, "reg", escc_reg, 2);
        t.set_int(n.escc, "#address-cells", 1);
        t.set_int(n.escc, "#size-cells", 1);
        const u32 a_reg[2] = {base + 0x1020, 0x20}, b_reg[2] = {base + 0x1000, 0x20};
        n.escc_a = cell(t, n.escc, n.pic, "ch-a", "serial", a_reg, 2, irq_a, 1);
        n.escc_b = cell(t, n.escc, n.pic, "ch-b", "serial", b_reg, 2, irq_b, 1);
        t.set_string(n.escc_a, "compatible", "chrp,es4");
        t.set_string(n.escc_b, "compatible", "chrp,es5");
    }
    const u32 nvram_reg[2] = {0x60000, 0x20000};
    n.nvram = cell(t, n.macio, n.pic, "nvram", "nvram", nvram_reg, 2, nullptr, 0);

    // --- display (our framebuffer device; docs/hardware/framebuffer.md).
    // Under /pci, as PCI device display_device with assigned-addresses:
    // Mac OS maps a device's memory and gives its driver logical addresses
    // (AAPL,address) from them. As a root node with physical "reg" it stayed
    // unmapped in Mac OS 9.2.1 and our driver's first register read faulted.
    // There is no configuration space behind it (no vendor ID of ours to
    // give), and Mac OS needs none: it maps from these properties. The
    // other properties are the ones Linux offb reads.
    if (m.vram_size) {
        char dunit[9];
        hex_unit(dunit, m.display_device);
        n.display = t.add_node(n.pci, "display", dunit);
        t.set_string(n.display, "device_type", "display");
        t.set_string(n.display, "compatible", "display");
        const u32 bar0 = pci_hi(kMem32, 0, m.display_device, 0, 0x10);
        const u32 bar1 = pci_hi(kMem32, 0, m.display_device, 0, 0x14);
        // reg (PCI binding): configuration space, then each BAR's size.
        const u32 reg[15] = {pci_hi(kConfig, 0, m.display_device, 0, 0), 0, 0, 0, 0,
                             bar0, 0, 0, 0, m.vram_bar,
                             bar1, 0, 0, 0, m.regs_size};
        t.set_ints(n.display, "reg", reg, m.regs_size ? 15 : 10);
        // assigned-addresses: where the firmware put the BARs (n: not relocatable).
        const u32 display_assigned[10] = {bar0 | 0x80000000u, 0, m.vram_base, 0, m.vram_bar,
                                  bar1 | 0x80000000u, 0, m.regs_base, 0, m.regs_size};
        t.set_ints(n.display, "assigned-addresses", display_assigned, m.regs_size ? 10 : 5);
        t.set_int(n.display, "address", m.vram_base);
        t.set_int(n.display, "width", m.width);
        t.set_int(n.display, "height", m.height);
        t.set_int(n.display, "depth", m.depth);
        t.set_int(n.display, "linebytes", m.linebytes);
    }

    // --- aliases
    char path[256];
    t.path_of(n.disk[0], path, sizeof path);
    t.set_string(n.aliases, "hd", path);
    t.set_string(n.aliases, "ide0", path);
    t.path_of(n.disk[1], path, sizeof path);
    t.set_string(n.aliases, "ide1", path);
    t.path_of(n.macio, path, sizeof path);
    t.set_string(n.aliases, "mac-io", path);
    if (n.display != kNone) {
        t.path_of(n.display, path, sizeof path);
        t.set_string(n.aliases, "screen", path);
    }
    return t.ok();
}

} // namespace g3::of
