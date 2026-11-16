#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/check-core.sh — enforce the portable-core rules (docs/02-architecture.md):
#  1. core/ and platform/include/ must not include Circle headers.
#  2. core/ must not include C++ standard library headers (Circle builds with
#     -nostdinc++); only <stdint.h> and <stddef.h> are allowed.
#  3. Every core header must compile on its own with the bare-metal
#     cross-compiler under Circle-like flags (skipped if not installed).

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
fail=0

if grep -rnE '#\s*include\s*[<"]circle/' core platform/include 2>/dev/null; then
    echo "error: Circle headers used in portable code (above)" >&2
    fail=1
fi

# Allowed system headers in core/: stdint.h and stddef.h only.
if grep -rnE '#\s*include\s*<' core | grep -vE '<(stdint|stddef)\.h>'; then
    echo "error: core/ may only include <stdint.h> and <stddef.h> (above)" >&2
    fail=1
fi

CXX_CROSS="${PREFIX:-aarch64-none-elf-}g++"
if command -v "$CXX_CROSS" >/dev/null; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    while IFS= read -r -d '' hdr; do
        echo "#include \"$hdr\"" > "$tmp/t.cpp"
        if ! "$CXX_CROSS" -std=c++17 -ffreestanding -nostdinc++ -fno-exceptions -fno-rtti \
                -mcpu=cortex-a72 -Wall -Wextra -Werror -I "$ROOT" -c "$tmp/t.cpp" -o "$tmp/t.o"; then
            echo "error: $hdr does not compile freestanding for AArch64" >&2
            fail=1
        fi
    done < <(find core -name '*.h' -print0)
    echo "freestanding AArch64 header check: done"
else
    echo "note: $CXX_CROSS not found; skipping freestanding compile check"
fi

if [[ $fail -ne 0 ]]; then
    exit 1
fi
echo "core checks passed"
