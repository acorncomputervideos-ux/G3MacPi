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
