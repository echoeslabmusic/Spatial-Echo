#!/bin/bash
# Spatial Echo macOS CMake build wrapper.
# Usage: ./scripts/build.sh [release|debug] [vst3|au|standalone|headless|all]
#
# Build only: this script configures and builds CMake targets and writes
# nothing outside build/ (toolchain caches aside). Installing into the user
# plug-in folders is a separate explicit step:
#   ./scripts/install_macos_plugins.sh --format vst3|au|all

set -euo pipefail

CONFIG="${1:-release}"
TARGET="${2:-all}"
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
cd "$ROOT_DIR"
RELEASE_PRESET="macos-universal-release"
DEBUG_PRESET="macos-debug"
RELEASE_ARTIFACT_ROOT="${ROOT_DIR}/build/cmake/${RELEASE_PRESET}/SpatialEcho_artefacts/Release"
DEBUG_ARTIFACT_ROOT="${ROOT_DIR}/build/cmake/${DEBUG_PRESET}/SpatialEcho_artefacts/Debug"
DEBUG_BINARY_ROOT="${ROOT_DIR}/build/cmake/${DEBUG_PRESET}/Debug"

# Validate configuration
if [[ "$CONFIG" != "release" && "$CONFIG" != "debug" ]]; then
    echo "Error: Configuration must be 'release' or 'debug'"
    exit 1
fi

if [[ "$CONFIG" == "release" ]]; then
    CMAKE_PRESET="$RELEASE_PRESET"
    BUILD_CONFIG="Release"
else
    CMAKE_PRESET="$DEBUG_PRESET"
    BUILD_CONFIG="Debug"
fi

echo "============================================"
echo "Building Spatial Echo (${BUILD_CONFIG}, ${CMAKE_PRESET})"
echo "============================================"

configure_preset() {
    cmake --preset "$CMAKE_PRESET"
}

build_targets() {
    echo ""
    echo "Building CMake target(s): $*"
    cmake --build --preset "$CMAKE_PRESET" --target "$@"
}

reject_debug_host_plugin() {
    local format="$1"

    if [[ "$BUILD_CONFIG" == "Debug" ]]; then
        echo "Error: Debug ${format} builds are not part of the normal workflow."
        echo "Use Debug standalone/headless targets for development and Release VST3/AU for DAW testing."
        exit 1
    fi
}

configure_preset

BUILT_OUTPUTS=()
case "$TARGET" in
    vst3)
        reject_debug_host_plugin "VST3"
        build_targets "SpatialEcho_VST3"
        BUILT_OUTPUTS+=("VST3")
        ;;
    au)
        reject_debug_host_plugin "AU"
        build_targets "SpatialEcho_AU"
        BUILT_OUTPUTS+=("AU")
        ;;
    standalone)
        build_targets "SpatialEcho_Standalone"
        BUILT_OUTPUTS+=("Standalone")
        ;;
    headless)
        if [[ "$BUILD_CONFIG" != "Debug" ]]; then
            echo "Error: the release build uses the universal preset, which has no headless target."
            echo "./scripts/run_tests.sh builds and runs the Release headless runner."
            exit 1
        fi
        build_targets "SpatialEchoHeadlessTests"
        BUILT_OUTPUTS+=("Headless")
        ;;
    all)
        if [[ "$BUILD_CONFIG" == "Debug" ]]; then
            build_targets "SpatialEcho_Standalone" "SpatialEchoHeadlessTests"
            BUILT_OUTPUTS+=("Standalone")
            BUILT_OUTPUTS+=("Headless")
        else
            build_targets "SpatialEcho_VST3" "SpatialEcho_AU" "SpatialEcho_Standalone"
            BUILT_OUTPUTS+=("VST3")
            BUILT_OUTPUTS+=("AU")
            BUILT_OUTPUTS+=("Standalone")
        fi
        ;;
    *)
        echo "Error: Target must be 'vst3', 'au', 'standalone', 'headless', or 'all'"
        exit 1
        ;;
esac

echo ""
echo "============================================"
echo "Build complete!"
echo "============================================"
echo "Outputs built: ${BUILT_OUTPUTS[*]}"
if [[ " ${BUILT_OUTPUTS[*]} " == *" VST3 "* && "$BUILD_CONFIG" == "Release" ]]; then
    echo "VST3 artifact: ${RELEASE_ARTIFACT_ROOT}/VST3/Spatial-Echo.vst3"
fi
if [[ " ${BUILT_OUTPUTS[*]} " == *" AU "* && "$BUILD_CONFIG" == "Release" ]]; then
    echo "AU artifact:   ${RELEASE_ARTIFACT_ROOT}/AU/Spatial-Echo.component"
fi
if [[ " ${BUILT_OUTPUTS[*]} " == *" Standalone "* ]]; then
    if [[ "$BUILD_CONFIG" == "Release" ]]; then
        echo "Standalone:    ${RELEASE_ARTIFACT_ROOT}/Standalone/Spatial-Echo.app"
    else
        echo "Standalone:    ${DEBUG_ARTIFACT_ROOT}/Standalone/Spatial-Echo.app"
    fi
fi
if [[ " ${BUILT_OUTPUTS[*]} " == *" Headless "* ]]; then
    echo "Headless:      ${DEBUG_BINARY_ROOT}/Spatial-Echo-HeadlessTests"
fi
if [[ "$BUILD_CONFIG" == "Release" && ( " ${BUILT_OUTPUTS[*]} " == *" VST3 "* || " ${BUILT_OUTPUTS[*]} " == *" AU "* ) ]]; then
    echo ""
    echo "Install into the user plug-in folders (separate explicit step):"
    echo "  ./scripts/install_macos_plugins.sh --format vst3|au|all"
fi
