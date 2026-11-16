#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/build-pi.sh — build the bare-metal Raspberry Pi 400 image.
#
# Uses circle-stdlib (Circle + newlib + libstdc++), which DingusPPC needs.
# Requires the Arm GNU toolchain 15.2.rel1 (aarch64-none-elf-*) on PATH, plus
# make, git, curl and makeinfo (Debian/Ubuntu package "texinfo", needed by the
# newlib build). Produces build/sdcard/ ready to copy onto a FAT32 SD card.
#
#   tools/build-pi.sh                build kernel + assemble build/sdcard
#   tools/build-pi.sh --no-fw        skip downloading Raspberry Pi firmware files
#   tools/build-pi.sh --reconfigure  re-run circle-stdlib's configure (slow rebuild)
#   tools/build-pi.sh clean          clean the app (keeps circle-stdlib build)
#   tools/build-pi.sh distclean      clean everything including circle-stdlib

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STDLIB="$ROOT/third_party/circle-stdlib"
CIRCLE="$STDLIB/libs/circle"
APP="$ROOT/app/circle"
OUT="$ROOT/build/sdcard"
PREFIX="${PREFIX:-aarch64-none-elf-}"
JOBS="${JOBS:-$(nproc)}"

clean_app() {
    make -C "$APP" clean || true
    make -C "$ROOT/port/circle" clean || true
    find "$ROOT/platform" "$ROOT/port" "$ROOT/third_party/dingusppc" \
        \( -name '*.o' -o -name '*.d' \) -delete 2>/dev/null || true
    rm -rf "$OUT"
}

case "${1:-}" in
    clean)
        clean_app
        exit 0 ;;
    distclean)
        clean_app
        (cd "$STDLIB" && make mrproper) || true
        exit 0 ;;
esac

FETCH_FW=1
RECONFIGURE=0
for arg in "$@"; do
    case "$arg" in
        --no-fw) FETCH_FW=0 ;;
        --reconfigure) RECONFIGURE=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

command -v "${PREFIX}g++" >/dev/null || {
    echo "error: ${PREFIX}g++ not found on PATH (install the Arm GNU toolchain)" >&2
    exit 1
}
command -v makeinfo >/dev/null || {
    echo "error: makeinfo not found (install the 'texinfo' package)" >&2
    exit 1
}

# circle-stdlib's own submodules: only Circle and newlib are needed.
if [[ ! -f "$CIRCLE/Rules.mk" || ! -f "$STDLIB/libs/circle-newlib/configure" ]]; then
    echo "==> Initialising circle-stdlib submodules (circle, circle-newlib)"
    git -C "$ROOT" submodule update --init third_party/circle-stdlib
    git -C "$STDLIB" submodule update --init libs/circle libs/circle-newlib
fi

if [[ $RECONFIGURE -eq 1 || ! -f "$STDLIB/Config.mk" ]]; then
    echo "==> Configuring circle-stdlib (AArch64, Raspberry Pi 4/400)"
    (cd "$STDLIB" && ./configure -r 4 -p "$PREFIX" --kernel-max-size 8 >/dev/null)
fi

echo "==> Building newlib (first build takes a few minutes)"
(cd "$STDLIB" && make -s newlib >/dev/null)

# Not circle-stdlib's 'make circle': that also builds the Wi-Fi add-on, which
# needs a further submodule we do not use.
echo "==> Building Circle libraries and add-ons"
(cd "$CIRCLE" && MAKE="make -j$JOBS" ./makeall --nosample >/dev/null)
for addon in SDCard fatfs qemu; do
    make -s -C "$CIRCLE/addon/$addon" -j"$JOBS"
done

echo "==> Building armstub8-rpi4.bin"
make -s -C "$CIRCLE/boot/armstub" armstub8-rpi4.bin

echo "==> Building DingusPPC + port layer (libg3dppc.a)"
# DingusPPC builds at its upstream warning level; keep the console readable.
mkdir -p "$ROOT/build"
make -s -C "$ROOT/port/circle" -j"$JOBS" > "$ROOT/build/dppc-build.log" 2>&1 || {
    grep -E "error" "$ROOT/build/dppc-build.log" >&2; exit 1; }
echo "    ($(grep -c 'warning:' "$ROOT/build/dppc-build.log") upstream warnings, see build/dppc-build.log)"
test -f "$ROOT/port/circle/libg3dppc.a"

echo "==> Building G3MacPi kernel"
make -s -C "$APP" -j"$JOBS"

echo "==> Assembling $OUT"
mkdir -p "$OUT"
cp "$APP/kernel8-rpi4.img" "$OUT/"
cp "$CIRCLE/boot/armstub/armstub8-rpi4.bin" "$OUT/"
cp "$ROOT/sdcard/config.txt" "$ROOT/sdcard/cmdline.txt" "$OUT/"
# DingusPPC CPU test vectors (plan step P2): the kernel runs them at boot.
mkdir -p "$OUT/g3macpi/tests"
cp "$ROOT"/third_party/dingusppc/cpu/ppc/test/{ppcinttests,ppcfloattests,ppcdisasmtest}.csv \
   "$OUT/g3macpi/tests/"

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
