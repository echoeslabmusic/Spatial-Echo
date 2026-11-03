#!/bin/bash
# Runs one headless test, measurement or fixture by name and prints its result.
# Usage: ./scripts/run_test.sh <TestName> [--debug] [--no-build]
#
# Uses the Release headless runner unless --debug is given;
# SPATIAL_ECHO_HEADLESS_RUNNER points at another runner. Outputs go to
# SPATIAL_ECHO_TEST_ARTIFACT_DIR when set; otherwise the runner makes a
# temporary directory and prints its path if anything was written.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
CONFIG="release"
TEST_NAME=""
BUILD_FIRST=1

usage() {
    echo "Usage: $0 <TestName> [--debug] [--no-build]"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --debug)
            CONFIG="debug"
            shift
            ;;
        --no-build)
            BUILD_FIRST=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "Error: unknown option '$1'"
            usage
            exit 2
            ;;
        *)
            if [ -n "$TEST_NAME" ]; then
                echo "Error: only one test name can be provided"
                usage
                exit 2
            fi
            TEST_NAME="$1"
            shift
            ;;
    esac
done

if [ -z "$TEST_NAME" ]; then
    echo "Error: test name is required"
    usage
    exit 2
fi

if [ "$CONFIG" = "debug" ]; then
    PRESET="macos-debug"
    DEFAULT_RUNNER="${ROOT_DIR}/build/cmake/macos-debug/Debug/Spatial-Echo-HeadlessTests"
else
    PRESET="macos-release-headless"
    DEFAULT_RUNNER="${ROOT_DIR}/build/cmake/macos-release-headless/Release/Spatial-Echo-HeadlessTests"
fi
RUNNER="${SPATIAL_ECHO_HEADLESS_RUNNER:-$DEFAULT_RUNNER}"

if [ "$BUILD_FIRST" -eq 1 ]; then
    cmake --preset "$PRESET"
    cmake --build --preset "$PRESET" --target SpatialEchoHeadlessTests
fi
if [ ! -x "$RUNNER" ]; then
    echo "Error: headless runner not found at ${RUNNER}"
    echo "Build it with: cmake --preset ${PRESET} && cmake --build --preset ${PRESET} --target SpatialEchoHeadlessTests"
    exit 2
fi

set +e
"$RUNNER" --test "$TEST_NAME"
exit_code=$?
set -e

echo "exit=${exit_code}"
exit "$exit_code"
