#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tools/run-dppc-cpu-tests.sh <dppc_cpu_tests binary> [log file]
#
# Runs DingusPPC's CPU test suite (it reads its CSV files from the current
# directory, so we run it inside third_party/dingusppc/cpu/ppc/test) and
# compares the result with tests/dingusppc/cpu_baseline.env.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
LOG="${2:-$ROOT/build/dppc_cpu_tests.log}"
mkdir -p "$(dirname "$LOG")"
# shellcheck source=/dev/null
source "$ROOT/tests/dingusppc/cpu_baseline.env"

(cd "$ROOT/third_party/dingusppc/cpu/ppc/test" && "$BIN") > "$LOG" 2>&1 || true

tested=$(sed -n 's/^--> Tested instructions: \([0-9]*\).*/\1/p' "$LOG")
failed=$(sed -n 's/^--> Failed: \([0-9]*\).*/\1/p' "$LOG")
disasm_failed=$(sed -n 's/^Tested [0-9]* instructions\. Failed: \([0-9]*\)\..*/\1/p' "$LOG")

echo "DingusPPC CPU tests: tested=${tested:-?} failed=${failed:-?} (baseline max $MAX_FAILED)," \
     "disassembler failed=${disasm_failed:-?} (max $MAX_DISASM_FAILED). Full log: $LOG"

if [[ -z "$tested" || -z "$failed" || -z "$disasm_failed" ]]; then
    echo "error: could not parse test output (did the test program crash?)" >&2
    tail -20 "$LOG" >&2
    exit 1
fi
status=0
if [[ "$tested" -ne "$EXPECTED_TESTED" ]]; then
    echo "error: expected $EXPECTED_TESTED tests, ran $tested" >&2; status=1
fi
if [[ "$failed" -gt "$MAX_FAILED" ]]; then
    echo "error: REGRESSION: $failed failures > baseline $MAX_FAILED" >&2; status=1
elif [[ "$failed" -lt "$MAX_FAILED" ]]; then
    echo "note: fewer failures than baseline — lower MAX_FAILED in tests/dingusppc/cpu_baseline.env"
fi
if [[ "$disasm_failed" -gt "$MAX_DISASM_FAILED" ]]; then
    echo "error: REGRESSION: $disasm_failed disassembler failures" >&2; status=1
fi
exit $status
