# SD card contents

The CI workflow `pi-image` (and `tools/build-pi.sh` locally) produces the
directory `build/sdcard/` with everything needed. To make a bootable card:

1. Format the SD card as **FAT32** (one partition; MBR partition table).
2. Copy **all files** from the artifact / `build/sdcard/` to the card's root.
3. Insert it in the Raspberry Pi 400 and power on.

| File | Origin |
|---|---|
| `start4.elf`, `fixup4.dat`, `bcm2711-rpi-400.dtb`, `LICENCE.broadcom` | Raspberry Pi firmware (revision pinned by Circle's `boot/Makefile`) |
| `armstub8-rpi4.bin` | Built from Circle `boot/armstub` |
| `config.txt`, `cmdline.txt` | This directory |
| `kernel8-rpi4.img` | G3MacPi |

`cmdline.txt` holds Circle kernel options (see `third_party/circle/doc/cmdline.txt`),
e.g. `loglevel=4` (debug) or `width=1280 height=720`.

## Settings menu and your files

At start-up the emulator shows its settings menu (F12 switches between the
menu and the log view; Ctrl+Alt+Delete reboots). On first start it creates:

| Folder / file | Put there |
|---|---|
| `/g3macpi/rom/` | the Mac OS ROM file (from your own Mac OS 9 install) |
| `/g3macpi/disks/` | CD images (`.iso`, `.toast`, `.cdr`) and hard-disk images |
| `/g3macpi/g3macpi.ini` | written by "Save settings" (docs/machine-config.md) |

The menu sets memory, the ROM file, the four ATA drive positions (CD-ROM
or hard disk, with an image from `/g3macpi/disks`), the start-up disk and
the screen mode. The Blue & White G3 has no floppy drive, so the floppy
line is greyed out. Starting the Mac from the menu is not in this build
yet: the emulator core does not run on the Pi so far.
