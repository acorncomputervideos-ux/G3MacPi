# Building and running

## From the iPad (normal workflow)

1. Push a commit (or run the **pi-image** workflow manually from the GitHub
   website/app: *Actions → pi-image → Run workflow*).
2. When it finishes, open the run and download the **g3macpi-sdcard** artifact
   (a zip). In the Files app, tap it to unzip.
3. Insert the SD card into the iPad with a USB-C/Lightning SD reader. The card
   must be **FAT32**. Copy every file from the unzipped folder to the card's
   root, replacing old files.
4. Eject, put the card in the Pi 400, power on.

The **host-tests** workflow runs all unit tests on every push; a red cross on
the commit means something broke.

## On a Linux machine (or in a Claude Code session)

```sh
git clone --recurse-submodules <repo-url>
cd G3MacPi

# Unit tests (needs cmake + g++ or clang++)
cmake -S . -B build/host -DG3_SANITIZE=ON
cmake --build build/host -j
./build/host/g3_unit_tests            # all tests
./build/host/g3_unit_tests common/    # tests whose name starts with "common/"

# Bare-metal image (needs the Arm GNU toolchain 15.2.rel1 aarch64-none-elf on PATH)
tools/build-pi.sh                     # result in build/sdcard/
tools/check-core.sh                   # portability rules
```

## What you should see (Milestone 1 kernel)

- The green ACT LED blinks 3 times.
- HDMI: the blue G3MacPi banner, version and git revision, then log lines:
  host information including **`ARM clock: 1800 MHz (max 1800 MHz), SoC .. C`**,
  **`Core self-test: PASSED`**, `Log written to SD:/g3macpi/logs/boot.log`,
  and `USB keyboard attached` (the Pi 400's own keyboard).
- **Keyboard test:** every key shows two lines, e.g.
  `Key down usage 0x04  A` and `Key up   usage 0x04  A`.
  Modifiers appear as `LCtrl`, `LShift`, `LAlt`, `LGUI` etc.
- **Mouse test** (plug in a USB mouse): `USB mouse attached (...)`, then
  lines like `Mouse: dx 12 dy -3 wheel 0  buttons L- R- M-  (8 reports)`.
  Moving right gives positive dx, moving down positive dy; held buttons show `*`.
- **Ctrl+Alt+Delete** writes the log file again and reboots.
- On the SD card afterwards (readable on the iPad): `g3macpi/logs/boot.log`
  (this run) and `g3macpi/logs/boot-prev.log` (the run before).
- UART (optional, GPIO14 = pin 8 TX, GPIO15 = pin 10 RX, GND = pin 6;
  115200 8N1, **3.3 V adapter only**): the same log lines.

Use the micro-HDMI port next to the power connector (HDMI0).
