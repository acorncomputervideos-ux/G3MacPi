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

// The root node's "copyright" (see bw_device_tree.cpp for why it's there).
constexpr const char* kAppleCopyrightNotice = "Copyright 1983-1999 Apple Computer, Inc. All Rights Reserved";

struct BwMachine {
    u32 ram_size = 0;
    u32 cpu_hz = 350000000;
    u32 bus_hz = 100000000;
    u32 pvr = 0;
    u32 macio_base = 0x80800000u;   // mac-io BAR as the firmware assigns it
    u32 bridge_device = 0x0D;       // DEC 21154 on bus 0
    u32 macio_device = 0x05;        // mac-io on bus 1 (behind the bridge)
    // The display device (0 = none) and its current mode.
    u32 vram_base = 0, vram_size = 0;
    u32 regs_base = 0, regs_size = 0;  // its register block (second "reg" entry; 0 = none)
    u32 width = 0, height = 0, depth = 0, linebytes = 0;
};

struct BwNodes {
    Phandle root, cpus, cpu, memory, chosen, aliases, options, openprom, packages, console, rom;
    Phandle pci, bridge, macio, cuda, nvram;
    Phandle ata[2], disk[2];
    Phandle display;  // kNone without a display
};

// Builds the whole tree into `t` (which must be empty). False if the tree
// ran out of space.
bool build_bw_device_tree(DeviceTree& t, const BwMachine& m, BwNodes& out);

} // namespace g3::of

#endif // G3_CORE_FIRMWARE_OPENFIRM_BW_DEVICE_TREE_H
