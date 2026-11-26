#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/build-pi.sh — build the bare-metal Raspberry Pi 400 image.
#
# Requires the Arm GNU toolchain (aarch64-none-elf-*) on PATH, plus make,
# git and curl. Produces build/sdcard/ ready to copy onto a FAT32 SD card.
#
#   tools/build-pi.sh            build kernel + assemble build/sdcard
#   tools/build-pi.sh --no-fw    skip downloading Raspberry Pi firmware files
#   tools/build-pi.sh clean      clean Circle and the app

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIRCLE="$ROOT/third_party/circle"
APP="$ROOT/app/circle"
OUT="$ROOT/build/sdcard"
PREFIX="${PREFIX:-aarch64-none-elf-}"
JOBS="${JOBS:-$(nproc)}"

if [[ "${1:-}" == "clean" ]]; then
    (cd "$CIRCLE" && ./makeall clean) || true
    make -C "$APP" clean || true
    rm -f "$ROOT"/platform/common/*.o "$ROOT"/platform/common/*.d
    find "$ROOT/core" \( -name '*.o' -o -name '*.d' \) -delete
    rm -f "$APP/ndrv_data.cpp"
    make -C "$CIRCLE/addon/SDCard" clean || true
    make -C "$CIRCLE/addon/fatfs" clean || true
    make -C "$CIRCLE/addon/wlan" clean || true
    make -C "$CIRCLE/addon/wlan/hostap/wpa_supplicant" -f Makefile.circle clean || true
    make -C "$CIRCLE/boot/armstub" clean || true
    rm -rf "$OUT"
    exit 0
fi

FETCH_FW=1
[[ "${1:-}" == "--no-fw" ]] && FETCH_FW=0

command -v "${PREFIX}g++" >/dev/null || {
    echo "error: ${PREFIX}g++ not found on PATH (install the Arm GNU toolchain)" >&2
    exit 1
}
[[ -f "$CIRCLE/Rules.mk" ]] || {
    echo "error: Circle submodule missing; run: git submodule update --init" >&2
    exit 1
}

# The kernel image holds the JIT's code buffers (app/circle/kernel.cpp,
# docs/cpu/jit-a64.md), so it is bigger than Circle's default limit of
# 2 MB. A bigger image overlaps the boot stack and dies before anything
# shows (only the firmware's rainbow screen).
KERNEL_MAX_MB=24  # the image holds the 16 MB code buffer (app/circle/kernel.cpp)

echo "==> Configuring Circle (AArch64, Raspberry Pi 4/400, multi-core, kernel up to ${KERNEL_MAX_MB} MB)"
OLD_CONFIG="$(cat "$CIRCLE/Config.mk" 2>/dev/null || true)"
(cd "$CIRCLE" && ./configure -r 4 -p "$PREFIX" --kernel-max-size "$KERNEL_MAX_MB" --multicore -f >/dev/null)
# Circle's makefiles do not depend on Config.mk: after a change (e.g. the
# kernel size limit, which sysinit checks at start-up and halts on) its
# objects must be rebuilt, or they keep the old values.
if [[ -n "$OLD_CONFIG" && "$OLD_CONFIG" != "$(cat "$CIRCLE/Config.mk")" ]]; then
    echo "==> Circle's configuration changed: rebuilding it from clean"
    (cd "$CIRCLE" && ./makeall clean >/dev/null) || true
    for addon in SDCard fatfs wlan; do make -s -C "$CIRCLE/addon/$addon" clean || true; done
    make -s -C "$CIRCLE/addon/wlan/hostap/wpa_supplicant" -f Makefile.circle clean || true
    make -s -C "$APP" clean || true
fi

echo "==> Building Circle libraries"
(cd "$CIRCLE" && MAKE="make -j$JOBS" ./makeall --nosample >/dev/null)

echo "==> Building Circle add-ons (SD card driver, FatFs, WiFi driver and WPA supplicant)"
make -s -C "$CIRCLE/addon/SDCard" -j"$JOBS"
make -s -C "$CIRCLE/addon/fatfs" -j"$JOBS"
# The WPA supplicant (a port of hostap) is a submodule of Circle's.
if [[ ! -f "$CIRCLE/addon/wlan/hostap/wpa_supplicant/Makefile.circle" ]]; then
    (cd "$CIRCLE" && git submodule update --init addon/wlan/hostap)
fi
make -s -C "$CIRCLE/addon/wlan" -j"$JOBS"
make -s -C "$CIRCLE/addon/wlan/hostap/wpa_supplicant" -f Makefile.circle -j"$JOBS"

echo "==> Building armstub8-rpi4.bin"
make -s -C "$CIRCLE/boot/armstub" armstub8-rpi4.bin

echo "==> Embedding our Mac OS display driver"
# Built by the host build (needs powerpc-linux-gnu-gcc): cmake --build build/host --target g3_ndrv
NDRV_GIVEN="${NDRV:-}"
NDRV="${NDRV:-$ROOT/build/host/guest/g3_ndrv.pef}"
# Always brought up to date (make rebuilds it only if its sources changed):
# building it only when missing embedded a stale driver.
if [[ -z "${NDRV_GIVEN:-}" && -d "$ROOT/build/host" ]]; then
    cmake --build "$ROOT/build/host" --target g3_ndrv >/dev/null 2>&1 ||
        echo "warning: could not rebuild $NDRV; embedding the existing file" >&2
fi
[[ -f "$NDRV" ]] || echo "warning: $NDRV not found: the kernel will have no display driver (Mac OS cannot draw)" >&2
python3 "$ROOT/tools/embed_ndrv.py" "$NDRV" "$APP/ndrv_data.cpp"

echo "==> Building G3MacPi kernel"
make -s -C "$APP" -j"$JOBS"

echo "==> Assembling $OUT"
mkdir -p "$OUT"
# The whole kernel (code, data and bss) must end below Circle's limit.
END=$("${PREFIX}nm" "$APP/kernel8-rpi4.elf" | awk '$3 == "_end" { print $1 }')
LIMIT=$(( 0x80000 + KERNEL_MAX_MB * 1024 * 1024 ))
if [[ -z "$END" || $(( 16#$END )) -gt $LIMIT ]]; then
    echo "error: the kernel ends at 0x${END:-?}, beyond Circle's limit 0x$(printf %X $LIMIT) (KERNEL_MAX_MB)" >&2
    exit 1
fi
echo "==> Kernel ends at 0x$END (limit 0x$(printf %X $LIMIT))"
cp "$APP/kernel8-rpi4.img" "$OUT/"
cp "$CIRCLE/boot/armstub/armstub8-rpi4.bin" "$OUT/"
cp "$ROOT/sdcard/config.txt" "$ROOT/sdcard/cmdline.txt" "$ROOT/sdcard/README.md" "$OUT/"
# The folders G3MacPi uses, ready to fill on a computer (the Pi also
# creates them when it can read the card).
mkdir -p "$OUT/g3macpi/rom" "$OUT/g3macpi/cd" "$OUT/g3macpi/hdd" "$OUT/g3macpi/disks" "$OUT/g3macpi/logs"
printf 'Put the "Mac OS ROM" file here (from the System Folder of your own Mac OS 9 CD or install).\r\n' > "$OUT/g3macpi/rom/README.txt"
printf 'Put CD images here (images of the whole disc, e.g. .iso).\r\n' > "$OUT/g3macpi/cd/README.txt"
printf 'Hard-disk images go here; the settings menu can make a new blank one.\r\n' > "$OUT/g3macpi/hdd/README.txt"
printf 'Other disk images (older setups); the menu lists them too.\r\n' > "$OUT/g3macpi/disks/README.txt"

if [[ $FETCH_FW -eq 1 ]]; then
    echo "==> Fetching Raspberry Pi firmware (revision pinned by Circle)"
    FW_REV="$(sed -n 's/^FIRMWARE ?= *//p' "$CIRCLE/boot/Makefile")"
    BASE="https://raw.githubusercontent.com/raspberrypi/firmware/$FW_REV/boot"
    for f in start4.elf fixup4.dat bcm2711-rpi-400.dtb LICENCE.broadcom; do
        curl -sSfL --retry 3 -o "$OUT/$f" "$BASE/$f" || {
            echo "error: failed to download $f from $BASE" >&2
            rm -f "$OUT/$f"
            exit 1
        }
    done
fi

if [[ $FETCH_FW -eq 1 ]]; then
    # The WiFi chip's firmware (Circle's WLAN driver loads it from
    # /firmware), at the revision Circle pins (addon/wlan/firmware/Makefile),
    # from Raspberry Pi OS's firmware-nonfree; its licence terms are in
    # debian/copyright there, copied alongside.
    echo "==> Fetching WiFi firmware (revision pinned by Circle)"
    WLAN_REV="$(sed -n 's/^FIRMWARE ?= *//p' "$CIRCLE/addon/wlan/firmware/Makefile")"
    WBASE="https://raw.githubusercontent.com/RPi-Distro/firmware-nonfree/$WLAN_REV"
    mkdir -p "$OUT/firmware"
    # Name on the card <- path in firmware-nonfree (as in Circle's Makefile).
    while read -r name src; do
        curl -sSfL --retry 3 -o "$OUT/firmware/$name" "$WBASE/debian/config/brcm80211/$src" || {
            echo "error: failed to download $src" >&2
            rm -f "$OUT/firmware/$name"
            exit 1
        }
    done <<'LIST'
brcmfmac43455-sdio.bin cypress/cyfmac43455-sdio-minimal.bin
brcmfmac43455-sdio.txt brcm/brcmfmac43455-sdio.txt
brcmfmac43455-sdio.clm_blob cypress/cyfmac43455-sdio.clm_blob
brcmfmac43456-sdio.bin brcm/brcmfmac43456-sdio.bin
brcmfmac43456-sdio.txt brcm/brcmfmac43456-sdio.txt
brcmfmac43456-sdio.clm_blob brcm/brcmfmac43456-sdio.clm_blob
LIST
    curl -sSfL --retry 3 -o "$OUT/firmware/LICENCE-firmware-nonfree.txt" "$WBASE/debian/copyright" || {
        echo "error: failed to download the WiFi firmware's licence" >&2
        exit 1
    }
fi

echo "==> Done:"
ls -l "$OUT"
