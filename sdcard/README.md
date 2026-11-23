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
| `/g3macpi/machine.state` | the Mac's clock, NVRAM and PRAM, kept between runs (see below) |

The menu sets memory, the ROM file, the four ATA drive positions (CD-ROM
or hard disk, with an image from `/g3macpi/disks`), the start-up disk and
the screen mode. The Blue & White G3 has no floppy drive, so the floppy
line is greyed out.

"Start the Mac" boots the emulated Mac with those settings. Its screen is
centred on HDMI, scaled up by a whole factor where it fits. The keyboard
and mouse go to the Mac. F12 opens the menu again and pauses the Mac:
insert or eject a CD there, then "Resume". Changes marked `*` apply
through "Restart the Mac". The UART log reports the emulation speed every
10 seconds.

Not yet tested on a Raspberry Pi; please report what you see. Known: the
Pi 400 has no battery clock, so the Mac's clock starts at 2001-01-01; the
emulation is several times slower than a real G3.

## The Mac's clock and settings (`machine.state`)

A real Mac keeps its clock and its NVRAM/PRAM (time zone, start-up disk,
sound volume and other control-panel settings) alive on a battery. The Pi
400 has no battery clock, so G3MacPi keeps them in `/g3macpi/machine.state`:

- written when the Mac stops (Shut Down, the menu's stop, Ctrl+Alt+Delete)
  and, while it runs, once a minute if NVRAM or PRAM changed or the clock
  moved on by five minutes;
- read at the next start. If the Pi knows the real time, the Mac uses it;
  otherwise the Mac's clock carries on from where it stopped (it lags by
  the time the Pi was off; set it in the Date & Time control panel).
- Mac OS writes its settings to NVRAM when you choose Shut Down or
  Restart, so use those rather than switching off, as on a real Mac.

Delete the file to start afresh (like zapping the PRAM). A damaged file
(wrong size or checksum) is ignored.
