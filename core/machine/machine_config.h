// SPDX-License-Identifier: GPL-3.0-or-later
//
// core/machine/machine_config.h — the user's machine configuration (RAM,
// ROM file, drives, start-up disk, display mode), and its text form, the
// g3macpi.ini file on the SD card.
//
// Freestanding: fixed-size buffers, no allocation.
//
// File format (docs/machine-config.md):
//
//   ; comment            # comment
//   [machine]
//   ram_mb = 256
//   rom = /g3macpi/rom/Mac OS ROM
//   boot = auto          ; auto | cdrom | disk
//   display = 1024x768x8 ; width x height x depth (8, 16, 32)
//   jit = no             ; yes: the CPU's block cache and generated code
//   [ata0.master]
//   type = cdrom         ; none | disk | cdrom
//   image = /g3macpi/disks/Mac OS 9.iso
//   readonly = yes       ; disks only; CD-ROMs are always read-only
//
// Drive positions, as on a real Blue & White: ata0 is the mac-io ATA
// channel (the CD-ROM bus: CD-ROM as master, often a Zip as slave), ata1
// the primary channel of the CMD646 Ultra ATA controller (the hard-disk
// bus). The B&W has no floppy drive: a [floppy] section is accepted and
// reported, but has no effect.

#ifndef G3_CORE_MACHINE_MACHINE_CONFIG_H
#define G3_CORE_MACHINE_MACHINE_CONFIG_H

#include "core/common/types.h"

namespace g3 {

struct MachineConfig {
    static constexpr u32 kPathMax = 160;   // bytes including the terminating zero
    static constexpr u32 kDrives = 4;      // ata0.master, ata0.slave, ata1.master, ata1.slave

    enum class DriveType : u8 { None, Disk, CdRom };
    enum class Boot : u8 { Auto, CdRom, Disk };

    struct Drive {
        DriveType type = DriveType::None;
        char image[kPathMax] = {};         // empty: no medium (an empty CD-ROM drive)
        bool read_only = false;
    };

    u32 ram_mb = 256;
    char rom[kPathMax] = "/g3macpi/rom/Mac OS ROM";
    Boot boot = Boot::Auto;
    u32 width = 1024, height = 768, depth = 8;
    // The CPU's decoded-block cache and AArch64 code generation
    // (docs/cpu/jit-a64.md). Off by default until measured on the Pi.
    bool jit = false;
    Drive drive[kDrives];

    MachineConfig();  // the defaults: 256 MB, a CD-ROM drive as ata0 master

    // Choices offered (and accepted from the file).
    static constexpr u32 kRamChoices[] = {64, 128, 192, 256, 384, 512};
    struct Mode { u32 width, height; };
    static constexpr Mode kModes[] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 870},
                                      {1280, 1024}, {1600, 1200}};
    static constexpr u32 kDepths[] = {8, 16, 32};

    static const char* drive_name(u32 i);        // "ata0.master" ...
    static const char* drive_label(u32 i);       // "ATA 0 master" ...
    static const char* type_name(DriveType t);   // "none" "disk" "cdrom"
    static const char* boot_name(Boot b);        // "auto" "cdrom" "disk"
    static bool valid_ram(u32 mb);
    static bool valid_mode(u32 width, u32 height, u32 depth);

    // Sets drive i's image (truncation is refused: false, unchanged).
    bool set_image(u32 i, const char* path);
};

// Result of parsing: the file is applied as far as it is valid; every line
// that was ignored is counted, and the first one is described.
struct ConfigParseReport {
    u32 ignored = 0;           // lines ignored (unknown, malformed or invalid)
    u32 first_line = 0;        // line number (from 1) of the first ignored line
    char first_reason[80] = {};
    bool floppy = false;       // a [floppy] section was present (no drive on the B&W)
};

// Parses `len` bytes of INI text into `config` (which keeps its values for
// anything the text does not set).
void parse_machine_config(const char* text, u32 len, MachineConfig& config, ConfigParseReport& report);

// The drive to start up from (0..kDrives-1), or kDrives if none: with
// boot = cdrom the first CD-ROM drive with a disc, with boot = disk the
// first hard disk with an image, with auto a hard disk first, else a CD.
// `has_image[i]`: drive i has a disc / image.
u32 choose_boot_drive(const MachineConfig& config, const bool has_image[MachineConfig::kDrives]);

// Writes the configuration as INI text (with a header comment). Returns the
// length written, or 0 if `cap` is too small.
u32 write_machine_config(const MachineConfig& config, char* out, u32 cap);

} // namespace g3

#endif // G3_CORE_MACHINE_MACHINE_CONFIG_H
