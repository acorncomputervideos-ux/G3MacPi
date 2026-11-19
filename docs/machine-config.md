# Machine configuration and the settings menu

The configuration lives on the SD card in `/g3macpi/g3macpi.ini`. The
settings menu edits and saves it; the host probe reads the same file with
`G3_CONFIG=file.ini` (image paths are then host paths).

Code: `core/machine/machine_config.*` (model, parser, writer) and
`platform/common/config_menu.*` (the menu). Tests:
`tests/unit/machine/test_machine_config.cpp`,
`tests/unit/platform/test_config_menu.cpp`.

## File format

```ini
; comments start with ; or # (also after a blank on a line)
[machine]
ram_mb = 256                   ; 64 128 192 256 384 512
rom = /g3macpi/rom/Mac OS ROM  ; the Mac OS ROM file (user-supplied)
boot = auto                    ; auto | cdrom | disk
display = 1024x768x8           ; 640x480 800x600 1024x768 1152x870 1280x1024 1600x1200; depth 8 16 32

[ata0.master]                  ; also ata0.slave, ata1.master, ata1.slave
type = cdrom                   ; none | disk | cdrom
image = /g3macpi/disks/Mac OS 9.iso
readonly = yes                 ; disks; CD-ROMs are always read-only
```

Invalid lines are ignored and reported (count, first line and reason);
everything valid still applies. Keys and words are case-insensitive.

- **Memory:** up to 512 MB, the firmware's current limit (two BATs map
  RAM in virtual mode; docs/firmware/virtual-mode.md). A real Blue & White
  takes up to 1 GB.
- **Drives:** the two mac-io ATA channels, master and slave. A real B&W
  has its CD-ROM as master on channel 0 (the default here) and its hard
  disk on a CMD646 controller, which is not emulated yet; channel 1 stands
  in for it.
- **Floppy:** the Blue & White G3 has no floppy drive. A `[floppy]` section
  is reported and ignored; the menu shows the line greyed out.
- **Hard disks and Mac OS:** Mac OS mounts an ATA hard disk only if it
  carries Apple's ATA driver (docs/storage/disk-format.md).

## The menu

Main page: Memory, ROM file, the four drive positions, Floppy (not
available), Start-up disk, Screen, Colours, Save settings, Start/Resume.
Up/Down move, Left/Right change a value, Enter opens a drive page or a
file picker, Esc goes back (on the main page: start or resume).

A drive page sets the type, picks the image (a file browser over the SD
card, folders first), ejects it and sets read-only. While the Mac runs,
inserting or ejecting a CD takes effect at once; memory, ROM, screen,
drive types and hard-disk images are marked `*` and apply at the next start.
