#!/usr/bin/env bash
set -euo pipefail

PROFILE=""
if [[ $# -eq 1 && ( "$1" == "s3" || "$1" == "c3" ) ]]; then
    TARGET=$1
elif [[ $# -eq 2 && ( "$1" == "s3" || "$1" == "c3" ) && "$2" == "--probe" ]]; then
    TARGET=$1
    PROFILE="probe"
elif [[ $# -eq 2 && ( "$1" == "s3" || "$1" == "c3" ) && "$2" == "--default" ]]; then
    TARGET=$1
    PROFILE="default"
else
    printf 'Usage: %s s3|c3 [--probe|--default]\n' "$0" >&2
    printf '  (no flag)  build the product profile (USB prototype on S3, selftest on C3)\n' >&2
    printf '  --probe    build the stage-3 ESP-NOW probe (receiver on S3, sender on C3)\n' >&2
    printf '  --default  same as no flag; kept explicit for scripts\n' >&2
    exit 2
fi

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

# The probe is a separate configuration, not a runtime mode: it replaces the
# interactive phases on C3 and the USB composite device on S3, so shipping it as a
# runtime switch would put the radio and the other subsystems in one binary.
SDKCONFIG_DEFAULTS="sdkconfig.defaults"
if [[ "$PROFILE" == "probe" ]]; then
    if [[ ! -f "$ROOT/firmware/$TARGET/sdkconfig.probe.defaults" ]]; then
        printf 'Probe profile not defined for %s\n' "$TARGET" >&2
        exit 1
    fi
    SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.probe.defaults"
    printf 'Building %s with the ESP-NOW probe profile\n' "$TARGET"
else
    printf 'Building %s with the product profile\n' "$TARGET"
fi

# A probe build writes to the same build directory as the product build. Clearing it
# is what prevents a stale product-only object (USB descriptors, ADC selftest) from
# being linked into a probe image after the configuration changes.
if [[ "$PROFILE" == "probe" && -d "$ROOT/firmware/$TARGET/build" ]]; then
    rm -rf -- "$ROOT/firmware/$TARGET/build"
fi

idf.py -C "$ROOT/firmware/$TARGET" -B "$ROOT/firmware/$TARGET/build" \
    -D "SDKCONFIG_DEFAULTS=$SDKCONFIG_DEFAULTS" build
