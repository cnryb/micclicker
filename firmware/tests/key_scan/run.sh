#!/usr/bin/env bash
# Host-side test of the C3 key scanner (ladder classification + debouncing).
#
# Compiles the same source the firmware builds (firmware/components/key_scan/src) with the
# host compiler. The voltage windows are the measured ones, so a window regression fails
# here rather than only on the board. Nothing here needs an ESP-IDF install or hardware.
#
# Usage:
#   ./firmware/tests/key_scan/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
COMPONENT="$ROOT/firmware/components/key_scan"
TESTDIR="$ROOT/firmware/tests/key_scan"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf '== building host harness ==\n'
cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$COMPONENT/include" \
    -o "$WORK/keyscantool" \
    "$TESTDIR/key_scan_tool.c" \
    "$COMPONENT/src/mic_key_scan.c"
printf '   ok\n\n'

"$WORK/keyscantool"
