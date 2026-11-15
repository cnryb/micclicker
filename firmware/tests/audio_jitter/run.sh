#!/usr/bin/env bash
# Host-side test of the receiver-side PCM ring and its clock-drift controller.
#
# Compiles the same source the firmware builds (firmware/components/audio_jitter/src)
# against stand-ins for the ESP-IDF and FreeRTOS headers, then exercises the cases the
# 30-minute acceptance run depends on: underrun before the first frame, overflow that
# discards the oldest audio, a fast producer that must not grow without bound, a slow
# producer that must not run dry, and concealment that decays instead of holding DC.
# Nothing here needs an ESP-IDF install or hardware.
#
# Usage:
#   ./firmware/tests/audio_jitter/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
COMPONENT="$ROOT/firmware/components/audio_jitter"
TESTDIR="$ROOT/firmware/tests/audio_jitter"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf '== building host harness ==\n'
# Two shim directories come first on the include path: the esp_err.h stand-in shared
# with the G.722 test, and this test's FreeRTOS.h. Neither is reachable from a
# firmware build.
cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$TESTDIR/hostshim" -I"$ROOT/firmware/tests/g722/hostshim" -I"$COMPONENT/include" \
    -o "$WORK/jittertool" \
    "$TESTDIR/jitter_tool.c" \
    "$TESTDIR/host_shim.c" \
    "$COMPONENT/src/mic_jitter.c"
printf '   ok\n\n'

"$WORK/jittertool"
