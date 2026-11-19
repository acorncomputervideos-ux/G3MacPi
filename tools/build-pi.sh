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
    rm -f "$ROOT"/core/machine/machine_config.o "$ROOT"/core/machine/machine_config.d
    make -C "$CIRCLE/addon/SDCard" clean || true
    make -C "$CIRCLE/addon/fatfs" clean || true
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

echo "==> Configuring Circle (AArch64, Raspberry Pi 4/400)"
(cd "$CIRCLE" && ./configure -r 4 -p "$PREFIX" -f >/dev/null)

echo "==> Building Circle libraries"
(cd "$CIRCLE" && MAKE="make -j$JOBS" ./makeall --nosample >/dev/null)

echo "==> Building Circle add-ons (SD card driver, FatFs)"
make -s -C "$CIRCLE/addon/SDCard" -j"$JOBS"
make -s -C "$CIRCLE/addon/fatfs" -j"$JOBS"

echo "==> Building armstub8-rpi4.bin"
make -s -C "$CIRCLE/boot/armstub" armstub8-rpi4.bin

echo "==> Building G3MacPi kernel"
make -s -C "$APP" -j"$JOBS"

echo "==> Assembling $OUT"
mkdir -p "$OUT"
cp "$APP/kernel8-rpi4.img" "$OUT/"
cp "$CIRCLE/boot/armstub/armstub8-rpi4.bin" "$OUT/"
cp "$ROOT/sdcard/config.txt" "$ROOT/sdcard/cmdline.txt" "$OUT/"

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

echo "==> Done:"
ls -l "$OUT"
