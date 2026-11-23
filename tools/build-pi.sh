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
KERNEL_MAX_MB=8

echo "==> Configuring Circle (AArch64, Raspberry Pi 4/400, kernel up to ${KERNEL_MAX_MB} MB)"
(cd "$CIRCLE" && ./configure -r 4 -p "$PREFIX" --kernel-max-size "$KERNEL_MAX_MB" -f >/dev/null)

echo "==> Building Circle libraries"
(cd "$CIRCLE" && MAKE="make -j$JOBS" ./makeall --nosample >/dev/null)

echo "==> Building Circle add-ons (SD card driver, FatFs)"
make -s -C "$CIRCLE/addon/SDCard" -j"$JOBS"
make -s -C "$CIRCLE/addon/fatfs" -j"$JOBS"

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
