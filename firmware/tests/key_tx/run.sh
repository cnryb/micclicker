#!/usr/bin/env bash
# Host-side test of the C3 key delivery state machine (retransmission, ACK matching,
# attempt limits, bounded queue).
#
# Compiles the same source the firmware builds (firmware/components/key_tx/src) with the
# host compiler. Nothing here needs an ESP-IDF install or hardware.
#
# Usage:
#   ./firmware/tests/key_tx/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
COMPONENT="$ROOT/firmware/components/key_tx"
TESTDIR="$ROOT/firmware/tests/key_tx"

if [[ $# -ne 0 ]]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf '== building host harness ==\n'
cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$COMPONENT/include" -I"$ROOT/firmware/tests/g722/hostshim" \
    -o "$WORK/keytxtool" \
    "$TESTDIR/key_tx_tool.c" \
    "$COMPONENT/src/mic_key_tx.c"
printf '   ok\n\n'

"$WORK/keytxtool"
