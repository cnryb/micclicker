#!/usr/bin/env bash
# Host-side test of the MicClicker ESP-NOW framing layer.
#
# Compiles the same sources the firmware builds (firmware/components/link_proto/src)
# with the host compiler and checks the wire layout, the rejection rules, the
# sequence accounting (gaps, late frames, duplicates, wraparound) and the session
# reset. Nothing here needs an ESP-IDF install or hardware.
#
# Usage:
#   ./firmware/tests/link_proto/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
COMPONENT="$ROOT/firmware/components/link_proto"
TESTDIR="$ROOT/firmware/tests/link_proto"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf '== building host harness ==\n'
# hostshim supplies a stand-in esp_err.h so the component compiles without ESP-IDF.
# It comes first on the include path and is never used by a firmware build.
cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$ROOT/firmware/tests/g722/hostshim" -I"$COMPONENT/include" \
    -o "$WORK/linktool" \
    "$TESTDIR/link_proto_tool.c" \
    "$COMPONENT/src/mic_link.c"
printf '   ok\n\n'

"$WORK/linktool"
