#!/bin/bash
# Runs the release plug-in validators against built or installed
# bundles: pluginval at strictness 10, Steinberg's VST3 validator, and the
# PluginHostCheck latency/bypass/state/bounds checks. The same script runs on
# macOS and in Git Bash on Windows; validator tools are fetched into
# build/tools on first use (CI caches that folder). Both tools are pinned:
# pluginval to a release whose archives are checked against the SHA-256 values
# below, and the VST3 SDK to a tag.
#
# Usage: ./scripts/release_validators.sh macos|windows <report-dir> <vst3-bundle> [au-component]
# Host check binary: build/SpatialEchoHostCheck[.exe] (override with SPATIAL_ECHO_HOST_CHECK).

set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "Usage: $0 macos|windows <report-dir> <vst3-bundle> [au-component]" >&2
    exit 2
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
PLATFORM="$1"
REPORTS="$2"
VST3="$3"
AU="${4:-}"
TOOLS="${ROOT_DIR}/build/tools"
mkdir -p "$TOOLS" "$REPORTS"

PLUGINVAL_VERSION="v1.0.4"
PLUGINVAL_DIR="${TOOLS}/pluginval-${PLUGINVAL_VERSION}"
VST3SDK_TAG="v3.8.1_build_84"
VST3SDK_DIR="${TOOLS}/vst3sdk-${VST3SDK_TAG}"

case "$PLATFORM" in
    macos)
        PLUGINVAL_ZIP="pluginval_macOS.zip"
        PLUGINVAL_SHA256="3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f"
        PLUGINVAL="${PLUGINVAL_DIR}/pluginval.app/Contents/MacOS/pluginval"
        HOST_CHECK="${SPATIAL_ECHO_HOST_CHECK:-${ROOT_DIR}/build/SpatialEchoHostCheck}"
        EXE=""
        ;;
    windows)
        PLUGINVAL_ZIP="pluginval_Windows.zip"
        PLUGINVAL_SHA256="c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab"
        PLUGINVAL="${PLUGINVAL_DIR}/pluginval.exe"
        HOST_CHECK="${SPATIAL_ECHO_HOST_CHECK:-${ROOT_DIR}/build/SpatialEchoHostCheck.exe}"
        EXE=".exe"
        ;;
    *)
        echo "Unknown platform: $PLATFORM" >&2
        exit 2
        ;;
esac

native_path() {
    if [ "$PLATFORM" = windows ]; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

if [ ! -x "$PLUGINVAL" ]; then
    echo "==> Fetching pluginval ${PLUGINVAL_VERSION}"
    PLUGINVAL_ARCHIVE="${TOOLS}/pluginval-${PLUGINVAL_VERSION}-${PLUGINVAL_ZIP}"
    curl -fsSL -o "$PLUGINVAL_ARCHIVE" \
        "https://github.com/Tracktion/pluginval/releases/download/${PLUGINVAL_VERSION}/${PLUGINVAL_ZIP}"
    actual_sha256="$(sha256_of "$PLUGINVAL_ARCHIVE")"
    if [ "$actual_sha256" != "$PLUGINVAL_SHA256" ]; then
        rm -f "$PLUGINVAL_ARCHIVE"
        echo "pluginval archive SHA-256 mismatch: expected ${PLUGINVAL_SHA256}, got ${actual_sha256}" >&2
        exit 1
    fi
    rm -rf "$PLUGINVAL_DIR"
    unzip -q -o "$PLUGINVAL_ARCHIVE" -d "$PLUGINVAL_DIR"
fi

VST3_VALIDATOR="$(find "${VST3SDK_DIR}/build/bin" -type f -name "validator${EXE}" 2>/dev/null | head -1 || true)"
if [ -z "$VST3_VALIDATOR" ]; then
    echo "==> Building Steinberg VST3 validator (${VST3SDK_TAG})"
    rm -rf "$VST3SDK_DIR"
    git clone --quiet --depth 1 --branch "$VST3SDK_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/steinbergmedia/vst3sdk.git "$VST3SDK_DIR"
    cmake -S "$VST3SDK_DIR" -B "${VST3SDK_DIR}/build" -DCMAKE_BUILD_TYPE=Release \
        -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF > "${TOOLS}/vst3sdk-build.log" 2>&1
    cmake --build "${VST3SDK_DIR}/build" --config Release --target validator >> "${TOOLS}/vst3sdk-build.log" 2>&1
    VST3_VALIDATOR="$(find "${VST3SDK_DIR}/build/bin" -type f -name "validator${EXE}" | head -1)"
fi

failures=0
run() {
    local name="$1"; shift
    echo "==> ${name}"
    if "$@" > "${REPORTS}/${name}.log" 2>&1; then
        echo "    ok: ${name}"
    else
        echo "    FAILED: ${name} (see ${REPORTS}/${name}.log)"
        tail -30 "${REPORTS}/${name}.log"
        failures=$((failures + 1))
    fi
}

run pluginval-vst3 "$PLUGINVAL" --strictness-level 10 --validate "$(native_path "$VST3")"
run vst3-validator "$VST3_VALIDATOR" -e "$(native_path "$VST3")"
run hostcheck-vst3 "$HOST_CHECK" --format VST3 --plugin "$(native_path "$VST3")" \
    --report "${REPORTS}/hostcheck-vst3.json"

if [ -n "$AU" ]; then
    run pluginval-au "$PLUGINVAL" --strictness-level 10 --validate "$AU"
    run hostcheck-au "$HOST_CHECK" --format AudioUnit --plugin "AudioUnit:Effects/aufx,SpEc,EchL" \
        --report "${REPORTS}/hostcheck-au.json"
fi

if [ "$failures" -ne 0 ]; then
    echo "${failures} validator(s) failed"
    exit 1
fi
echo "All validators passed"
