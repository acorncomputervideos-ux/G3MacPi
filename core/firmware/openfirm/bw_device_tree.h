// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/firmware/openfirm/bw_device_tree.h — the device tree our firmware
// presents for the Blue & White G3 (PowerMac1,1), built from the emulated
// machine's configuration.
//
// Each property's source is listed in docs/firmware/bw-device-tree.md:
// what Linux needs to find this hardware (read from its source, facts
// only), the PowerPC 750 manual, the IEEE 1275 PCI binding, and the
// emulator's own layout. Values only a real B&W would show are marked
// "to verify" there — a device-tree dump of a real machine replaces them.

#ifndef G3_CORE_FIRMWARE_OPENFIRM_BW_DEVICE_TREE_H
#define G3_CORE_FIRMWARE_OPENFIRM_BW_DEVICE_TREE_H

#include "core/common/types.h"
#include "core/firmware/openfirm/device_tree.h"

namespace g3::of {

// The windows the PCI-to-PCI bridge forwards to bus 1: I/O 0x1000-0x1FFF
// (the CMD646's BARs) and the 1 MB-aligned memory range around mac-io's
// BAR. The firmware programs them into the bridge; the device tree's
// bridge "ranges" describe the same.
struct BridgeWindows {
    u32 io_base, io_size, mem_base, mem_size;
};
constexpr BridgeWindows bridge_windows(u32 macio_base)
{
    return BridgeWindows{0x1000, 0x1000, macio_base & 0xFFF00000u,
                         ((macio_base + 0x80000u + 0xFFFFFu) & 0xFFF00000u) - (macio_base & 0xFFF00000u)};
}

// The root node's "copyright" (see bw_device_tree.cpp for why it's there).
constexpr const char* kAppleCopyrightNotice = "Copyright 1983-1999 Apple Computer, Inc. All Rights Reserved";

// The interrupt-controller node's reg, relative to the PIC's register window
// (Paddington::kPicOffset): the two banks Mac OS addresses from it.
constexpr u32 kPicNodeOffset = 0x10, kPicNodeSize = 0x20;

struct BwMachine {
    u32 ram_size = 0;
    u32 cpu_hz = 350000000;
    u32 bus_hz = 100000000;
    u32 pvr = 0;
    u32 macio_base = 0x80800000u;   // mac-io BAR as the firmware assigns it
    u32 bridge_device = 0x0D;       // DEC 21154 on bus 0
    u32 macio_device = 0x05;        // mac-io on bus 1 (behind the bridge)
    // The CMD646 hard-disk controller on bus 1 and the PCI I/O addresses of
    // its five BARs (0 = no controller).
    u32 cmd646_device = 0x01;
    u32 cmd646_io[5] = {};
    // The display device (0 = none) and its current mode.
    u32 vram_base = 0, vram_size = 0;
    u32 regs_base = 0, regs_size = 0;  // its register block (second "reg" entry; 0 = none)
    u32 width = 0, height = 0, depth = 0, linebytes = 0;
};

struct BwNodes {
    Phandle root, cpus, cpu, memory, chosen, aliases, options, openprom, packages, console, rom;
    Phandle pci, bridge, macio, pic, cuda, adb, scsi, escc, escc_a, escc_b, nvram;
    Phandle ata[2], disk[2];
    Phandle cmd646, cmd646_channel, cmd646_disk;  // kNone without a CMD646
    Phandle display;  // kNone without a display
};

// Builds the whole tree into `t` (which must be empty). False if the tree
// ran out of space.
bool build_bw_device_tree(DeviceTree& t, const BwMachine& m, BwNodes& out);

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_BW_DEVICE_TREE_H
