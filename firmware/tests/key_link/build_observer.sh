#!/usr/bin/env bash
# Build the host-side keyboard observer used by the stage-5 acceptance run.
#
# macOS only: it uses IOHIDManager. Nothing here touches the firmware, so it works whether
# or not ESP-IDF is installed.
#
# Usage:
#   ./firmware/tests/key_link/build_observer.sh [output-path]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TESTDIR="$ROOT/firmware/tests/key_link"
OUT="${1:-$TESTDIR/key_observer}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf 'the keyboard observer needs macOS IOHIDManager; this host is %s\n' "$(uname -s)" >&2
    exit 2
fi

cc -O2 -std=c99 -Wall -Wextra -Werror \
    -framework CoreFoundation -framework IOKit \
    -o "$OUT" "$TESTDIR/key_observer.c"
printf 'built %s\n' "$OUT"
