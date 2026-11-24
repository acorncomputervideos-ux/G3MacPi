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
| `/g3macpi/rom/` | the Mac OS ROM file (from your own Mac OS 9 install); optional with a Mac OS 9 CD image in the CD drive |
| `/g3macpi/cd/` | CD images (`.iso`, `.toast`, `.cdr`); the file picker starts here for CD-ROM drives |
| `/g3macpi/hdd/` | hard-disk images; the picker starts here for hard disks, and new blank disks are made here |
| `/g3macpi/disks/` | any images (kept for older setups; still usable from the picker) |
| `/g3macpi/g3macpi.ini` | written by "Save settings" (docs/machine-config.md) |
| `/g3macpi/machine.state` | the Mac's clock, NVRAM and PRAM, kept between runs (see below) |

The menu sets memory, the ROM file, the four ATA drive positions (CD-ROM
or hard disk, with an image from `/g3macpi/disks`), the start-up disk and
the screen mode. The Blue & White G3 has no floppy drive, so the floppy
line is greyed out.

**The Mac OS ROM file.** The Blue & White G3 starts Mac OS from a file,
"Mac OS ROM", in the System Folder of Mac OS 9 (not from a ROM chip: a
"…Power Mac G3 desktop.ROM" dump of an older beige G3's chip will not
work). If the ROM file in the settings is missing or is not that file,
G3MacPi takes "Mac OS ROM" from the System Folder of the CD image in the
CD drive, so a Mac OS 9 CD image alone is enough.

**Speed.** The Mac's time follows real time: when the Pi emulates fewer
instructions per second than a 350 MHz G3, the Mac runs as a slower G3
(the log's "effective clock") instead of in slow motion, so the pointer,
clock and animations keep their pace. For more speed try "CPU emulation:
JIT" in the menu (`jit = yes`): it compiles PowerPC code to ARM code; it
is new, so if the Mac misbehaves, switch it back. The web page's Status
and the log show the speed (MIPS).

**Start-up.** The Pi goes straight to the Mac: a start-up screen, then
Mac OS (no text log on the screen; it goes to the serial port and to
`/g3macpi/logs/boot.log`). Hold **F12** or **Esc** while the start-up
screen shows to get the settings menu instead; F12 also opens it later.
If the Mac can't start (no SD card, no ROM file, ...) the menu shows with
the reason. "At power-on" in the menu (`autostart` in the settings) can
make it always open the menu. To see the log on the screen while
starting, add ` bootlog=1` to `cmdline.txt` (same line).

"Start the Mac" boots the emulated Mac with those settings. Its screen is
centred on HDMI, scaled up by a whole factor where it fits. The keyboard
and mouse go to the Mac. F12 opens the menu again and pauses the Mac:
insert or eject a CD there, then "Resume". Changes marked `*` apply
through "Restart the Mac". The UART log reports the emulation speed every
10 seconds.

Sound and Ethernet (both on by default, "Sound" and "Ethernet" in the
menu): the Mac's sound goes to the HDMI port Circle drives (HDMI 0) and
its network to the Pi's Ethernet port, with the Pi's own Ethernet address.
Both are new and not yet working end to end in Mac OS: see
docs/hardware/davbus.md and docs/hardware/bmac.md.

Web interface: with an Ethernet cable in, open the address the menu shows
(`http://<the Pi's address>/`) to see the status, upload CD and disk
images, edit the settings and update the emulator itself (upload a new
kernel8-rpi4.img, then "Restart the Pi"). Set `web_password` on a shared
network. See docs/web-interface.md.

If the log says "The SD card could not be read" (or `emmc: ... failed`),
the Pi started but G3MacPi can't use the card: no settings, images or log
are read or written. Try another card, formatted FAT32 (the folders above
are on it already when you copy this whole package), and tell us the
card's make and size and the lines above the message.

First runs on a Pi 400 have been reported; please report what you see. Known: the
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

## Speed: clock and temperature

The emulator runs the Pi 400's ARM cores at full speed (1.8 GHz). Circle
slows them to 600 MHz when the chip is hotter than `socmaxtemp`
(`cmdline.txt`; Circle's default is 60 °C, G3MacPi sets 78, the most Circle
allows; the Pi firmware itself protects the chip from 80 °C). The UART log
shows the clock and temperature every 10 seconds and warns when the clock
is reduced. `config.txt` has an optional overclock (2.0 GHz), commented out.

## A new hard disk

In the menu, open a drive position, set Type to Hard disk, then go to
"New blank disk": Left/Right picks the size (256 MB to 4 GB; FAT32 files
can't reach 4 GB), Enter creates `/g3macpi/hdd/hdN.img` and puts it in the
drive. The disk is blank: start the Mac from the Mac OS CD, open Drive
Setup (in the CD's Utilities folder), initialise the disk, then install
Mac OS on it. (Drive Setup writes Apple's disk driver onto the disk; Mac
OS doesn't mount an ATA disk without it.)
