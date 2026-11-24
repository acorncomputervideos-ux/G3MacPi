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
jit = no                       ; yes: block cache + generated code (experimental, docs/cpu/jit-a64.md)
sound = yes                    ; built-in sound, played on HDMI (docs/hardware/davbus.md)
network = yes                  ; built-in Ethernet, on the Pi's Ethernet port (docs/hardware/bmac.md)
autostart = yes                ; at power-on start the Mac at once (no: show the settings menu)
web = yes                      ; the web interface (docs/web-interface.md)
web_password = something       ; optional: the web interface asks for it
wifi = no                      ; yes: the Pi's WiFi instead of its Ethernet port (docs/wifi.md)
wifi_name = My Network         ; the WiFi network (SSID), up to 32 bytes
wifi_password = secret123      ; WPA2 passphrase, 8-63 characters; leave out for an open network
wifi_country = GB              ; optional: two-letter country code (allowed channels)

[ata0.master]                  ; also ata0.slave, ata1.master, ata1.slave
type = cdrom                   ; none | disk | cdrom
image = /g3macpi/disks/Mac OS 9.iso
readonly = yes                 ; disks; CD-ROMs are always read-only
```

`sound`, `network`, `wifi`, `autostart` and `jit` are also in the menu
("Sound", "Ethernet", "Network port", "At power-on", "CPU emulation"); a
change applies when the Mac restarts (the network port: when the Pi
restarts). `jit` is off by default until it has been measured on a Pi.
The WiFi name and password are set in the file (or on the web page's
settings): a value ends at a `;` or `#` that follows a space, and spaces at
either end are dropped; a password may not contain `"`.

Invalid lines are ignored and reported (count, first line and reason);
everything valid still applies. Keys and words are case-insensitive.

- **Memory:** up to 512 MB, the firmware's current limit (two BATs map
  RAM in virtual mode; docs/firmware/virtual-mode.md). A real Blue & White
  takes up to 1 GB.
- **Drives:** as on a real Blue & White: `ata0` is the mac-io ATA channel
  (the CD-ROM bus; CD-ROM as master by default), `ata1` the CMD646's
  primary channel (the hard-disk bus; docs/hardware/cmd646.md). The menu
  calls them "CD bus" and "Hard disk bus".
- **Floppy:** the Blue & White G3 has no floppy drive. A `[floppy]` section
  is reported and ignored; the menu shows the line greyed out.
- **Hard disks and Mac OS:** Mac OS mounts an ATA hard disk only if it
  carries Apple's ATA driver (docs/storage/disk-format.md).

## The menu

On the Pi the menu is drawn as a classic Mac desktop: a grey desktop, a
white menu bar (the keys at its right), and a window with a pinstriped
title bar holding the settings, the selected row inverted
(`platform/common/menu_painter`). Its bold bitmap font was drawn for this
project in the spirit of the classic Mac system font
(`tools/font/make_ui_font.py`; Apple's fonts are not used). It is scaled
by a whole factor to the screen (2x on a 1080p screen).
`g3_menu_preview <prefix> [w h]` renders the pages to PPM files.

Main page: Memory, ROM file, the four drive positions, Floppy (not
available), Start-up disk, Screen, Colours, Save settings, Start/Resume.
Up/Down move, Left/Right change a value, Enter opens a drive page or a
file picker, Esc goes back (on the main page: start or resume).

A drive page sets the type, picks the image (a file browser over the SD
card, folders first; it starts in `/g3macpi/cd` for CD-ROM drives and
`/g3macpi/hdd` for hard disks), ejects it, sets read-only, and for hard
disks makes a new blank disk image ("New blank disk": 256 MB to 4 GB,
created as `/g3macpi/hdd/hdN.img`; sdcard/README.md). While the Mac runs,
inserting or ejecting a CD takes effect at once; memory, ROM, screen,
drive types and hard-disk images are marked `*` and apply at the next start.
