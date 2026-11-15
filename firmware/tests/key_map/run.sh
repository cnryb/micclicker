#!/usr/bin/env bash
# Host-side test of the shortcut mapping and the HID report state machine.
#
# Compiles the same source the firmware builds (firmware/components/key_map/src) with the
# host compiler. The NVS-facing half (key_store) is not covered here: it needs a real NVS
# partition, so it is exercised on the board and through the config HID path instead.
# Nothing here needs an ESP-IDF install or hardware.
#
# Usage:
#   ./firmware/tests/key_map/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
COMPONENT="$ROOT/firmware/components/key_map"
TESTDIR="$ROOT/firmware/tests/key_map"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf '== building host harness ==\n'
# The esp_err.h stand-in is shared with the G.722 and link_proto tests; it comes first on
# the include path and is never used by a firmware build.
cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$ROOT/firmware/tests/g722/hostshim" -I"$COMPONENT/include" \
    -o "$WORK/keymaptool" \
    "$TESTDIR/key_map_tool.c" \
    "$COMPONENT/src/mic_key_map.c"
printf '   ok\n\n'

"$WORK/keymaptool"
