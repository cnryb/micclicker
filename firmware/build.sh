#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ( "$1" != "s3" && "$1" != "c3" ) ]]; then
    printf 'Usage: %s s3|c3\n' "$0" >&2
    exit 2
fi

TARGET=$1
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v5.5.3}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.5_py3.13_env}"

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    printf 'ESP-IDF export.sh not found under %s\n' "$IDF_PATH" >&2
    exit 1
fi

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
VERSION="$(idf.py --version)"
if [[ "$VERSION" != "ESP-IDF v5.5.3" ]]; then
    printf 'Expected ESP-IDF v5.5.3, got %s\n' "$VERSION" >&2
    exit 1
fi

EXPECTED_IDF_COMMIT="2c211b236707889e8400c4dc5644dd5c4ee071e0"
ACTUAL_IDF_COMMIT="$(git -C "$IDF_PATH" rev-parse HEAD)"
if [[ "$ACTUAL_IDF_COMMIT" != "$EXPECTED_IDF_COMMIT" ]]; then
    printf 'Expected IDF commit %s, got %s\n' "$EXPECTED_IDF_COMMIT" "$ACTUAL_IDF_COMMIT" >&2
    exit 1
fi

if [[ "$TARGET" == "s3" ]]; then
    COMPILER=xtensa-esp-elf-gcc
else
    COMPILER=riscv32-esp-elf-gcc
fi
COMPILER_VERSION="$($COMPILER --version)"
if [[ "$COMPILER_VERSION" != *"esp-14.2.0_20251107"* ]]; then
    printf 'Unexpected compiler version for %s:\n%s\n' "$TARGET" "$COMPILER_VERSION" >&2
    exit 1
fi

# sdkconfig is generated input, not the source of truth. Recreate it so edits to
# sdkconfig.defaults cannot be masked by stale choices from an earlier build.
SDKCONFIG="$ROOT/firmware/$TARGET/sdkconfig"
if [[ -e "$SDKCONFIG" ]]; then
    RESOLVED_SDKCONFIG="$(realpath "$SDKCONFIG")"
    if [[ "$RESOLVED_SDKCONFIG" != "$ROOT/firmware/$TARGET/sdkconfig" ]]; then
        printf 'Refusing unexpected sdkconfig path: %s\n' "$RESOLVED_SDKCONFIG" >&2
        exit 1
    fi
    rm -- "$RESOLVED_SDKCONFIG"
fi

idf.py -C "$ROOT/firmware/$TARGET" build
