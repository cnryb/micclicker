#!/usr/bin/env bash
# Host-side G.722 conformance check for the vendored codec core.
#
# Compiles the same sources the firmware uses (firmware/components/g722/src) with
# the host compiler, then checks them against the golden vectors committed under
# firmware/tests/g722/vectors. Nothing here needs an ESP-IDF install or hardware.
#
# Usage:
#   ./firmware/tests/g722/run.sh              # contract + golden-vector checks
#   ./firmware/tests/g722/run.sh --itu DIR    # also report ITU vector status
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
COMPONENT="$ROOT/firmware/components/g722"
TESTDIR="$ROOT/firmware/tests/g722"
VECTORS="$TESTDIR/vectors"
ITU_ARGS=()

if [[ $# -ge 1 && "$1" == "--itu" ]]; then
    if [[ $# -ne 2 || ! -d "$2" ]]; then
        printf 'Usage: %s [--itu <extracted-vector-dir>]\n' "$0" >&2
        exit 2
    fi
    ITU_ARGS=(--itu "$2")
elif [[ $# -ne 0 ]]; then
    printf 'Usage: %s [--itu <extracted-vector-dir>]\n' "$0" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

printf '== building host harness ==\n'
# hostshim supplies a stand-in esp_err.h so the wrapper compiles without ESP-IDF.
# It comes first on the include path and is never used by a firmware build.
cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$TESTDIR/hostshim" -I"$COMPONENT/include" \
    -o "$WORK/g722tool" \
    "$TESTDIR/g722_tool.c" \
    "$COMPONENT/src/mic_g722.c" \
    "$COMPONENT/src/g722_encode.c" \
    "$COMPONENT/src/g722_decode.c" \
    -lm
printf '   ok\n\n'

# Confirm the committed vectors still match their recorded checksums, so a silently
# edited vector cannot pass as a baseline.
printf '== verifying vector checksums ==\n'
( cd "$VECTORS" && shasum -a 256 -c SHA256SUMS --quiet ) \
    && printf '   ok\n\n' \
    || { printf '   FAIL vector checksums do not match SHA256SUMS\n' >&2; exit 1; }

"$WORK/g722tool" --vectors "$VECTORS" ${ITU_ARGS[@]+"${ITU_ARGS[@]}"}
