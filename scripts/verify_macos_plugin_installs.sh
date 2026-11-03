#!/bin/bash
# Verifies macOS AU/VST3 install copies do not point at different binaries.
#
# Bundle names default to the current product identity via
# scripts/lib/macos_plugin_paths.sh, and each bundle's executable is derived
# from its own Info.plist CFBundleExecutable, so the verifier can never
# silently hash a wrong-identity binary path.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=lib/macos_plugin_paths.sh
source "${SCRIPT_DIR}/lib/macos_plugin_paths.sh"

FORMAT="all"
REQUIRE_USER=0
EXPECT_BUNDLE=""
SKIP_CODESIGN="${SPATIAL_ECHO_SKIP_CODESIGN:-0}"

usage() {
    cat <<USAGE
Usage: $0 [--format all|vst3|au] [--require-user] [--expect-bundle <path>]
          [--skip-codesign]

Checks user-level and system-level macOS plugin installs for stale duplicates.
Fails when both copies of the selected format exist but their plugin binaries
differ.

--expect-bundle <path> additionally requires the user-level install to match
the given source bundle exactly (executable hash plus Info.plist identifier,
executable name, and version). It implies --require-user and needs a single
--format (vst3 or au).
USAGE
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --format)
            if [ "$#" -lt 2 ]; then
                echo "Error: --format requires all, vst3, or au" >&2
                exit 1
            fi
            FORMAT="$2"
            shift 2
            ;;
        --require-user)
            REQUIRE_USER=1
            shift
            ;;
        --expect-bundle)
            if [ "$#" -lt 2 ]; then
                echo "Error: --expect-bundle requires a bundle path" >&2
                exit 1
            fi
            EXPECT_BUNDLE="$2"
            REQUIRE_USER=1
            shift 2
            ;;
        --skip-codesign)
            SKIP_CODESIGN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'" >&2
            usage >&2
            exit 1
            ;;
    esac
done

case "$FORMAT" in
    all|vst3|au) ;;
    *)
        echo "Error: --format must be all, vst3, or au" >&2
        exit 1
        ;;
esac

if [ -n "$EXPECT_BUNDLE" ]; then
    if [ "$FORMAT" = "all" ]; then
        echo "Error: --expect-bundle needs --format vst3 or --format au" >&2
        exit 1
    fi
    if [ ! -d "$EXPECT_BUNDLE" ]; then
        echo "Error: expected source bundle is missing: ${EXPECT_BUNDLE}" >&2
        exit 1
    fi
fi

verify_codesign() {
    local bundle="$1"

    if [ "$SKIP_CODESIGN" -eq 1 ]; then
        return 0
    fi

    codesign --verify --deep --strict "$bundle" >/dev/null 2>&1
}

print_bundle_state() {
    local label="$1"
    local bundle="$2"

    if [ ! -d "$bundle" ]; then
        echo "  ${label}: not installed (${bundle})"
        return 0
    fi

    echo "  ${label}: ${bundle}"

    local identity
    if identity="$(bundle_identity "$bundle" 2>/dev/null)"; then
        echo "    ${identity}"
    fi

    local hash
    if hash="$(hash_bundle_executable "$bundle" 2>/dev/null)"; then
        echo "    hash: ${hash}"
    else
        echo "    hash: unavailable (missing Info.plist, CFBundleExecutable, or binary)"
    fi
}

check_expected_source() {
    local name="$1"
    local user_bundle="$2"
    local source_bundle="$3"
    local failed=0

    local source_hash user_hash
    if ! source_hash="$(hash_bundle_executable "$source_bundle")"; then
        echo "ERROR: ${name} source bundle has no hashable executable: ${source_bundle}" >&2
        return 1
    fi
    if ! user_hash="$(hash_bundle_executable "$user_bundle" 2>/dev/null)"; then
        echo "ERROR: ${name} user-level install has no hashable executable: ${user_bundle}" >&2
        return 1
    fi

    if [ "$user_hash" != "$source_hash" ]; then
        echo "ERROR: ${name} user-level install does not match the source bundle." >&2
        echo "       source:    ${source_hash}" >&2
        echo "       installed: ${user_hash}" >&2
        failed=1
    fi

    local source_identity user_identity
    source_identity="$(bundle_identity "$source_bundle" 2>/dev/null || true)"
    user_identity="$(bundle_identity "$user_bundle" 2>/dev/null || true)"
    if [ "$source_identity" != "$user_identity" ]; then
        echo "ERROR: ${name} user-level install identity differs from the source bundle." >&2
        echo "       source:    ${source_identity:-(unreadable)}" >&2
        echo "       installed: ${user_identity:-(unreadable)}" >&2
        failed=1
    fi

    local source_archs user_archs
    source_archs="$(bundle_architectures "$source_bundle")"
    user_archs="$(bundle_architectures "$user_bundle")"
    if [ "$source_archs" != "$user_archs" ]; then
        echo "ERROR: ${name} user-level install architectures differ from the source bundle." >&2
        echo "       source:    ${source_archs}" >&2
        echo "       installed: ${user_archs}" >&2
        failed=1
    fi

    local source_triplet user_triplet
    source_triplet="$(bundle_au_triplet "$source_bundle")"
    user_triplet="$(bundle_au_triplet "$user_bundle")"
    if [ "$source_triplet" != "$user_triplet" ]; then
        echo "ERROR: ${name} user-level install AU component triplet differs from the source bundle." >&2
        echo "       source:    ${source_triplet}" >&2
        echo "       installed: ${user_triplet}" >&2
        failed=1
    fi

    if [ "$failed" -eq 0 ]; then
        echo "${name} install matches the source bundle (hash, identity, architectures ${user_archs}, AU triplet ${user_triplet})."
    fi

    return "$failed"
}

check_format() {
    local name="$1"
    local format_arg="$2"
    local user_bundle="$3"
    local system_bundle="$4"
    local failed=0

    echo "Checking ${name} installs..."
    print_bundle_state "user" "$user_bundle"
    print_bundle_state "system" "$system_bundle"

    if [ "$REQUIRE_USER" -eq 1 ] && [ ! -d "$user_bundle" ]; then
        echo "ERROR: ${name} user-level install is missing: ${user_bundle}" >&2
        failed=1
    fi

    local user_hash=""
    local system_hash=""
    if [ -d "$user_bundle" ]; then
        if ! user_hash="$(hash_bundle_executable "$user_bundle" 2>/dev/null)"; then
            echo "ERROR: ${name} user-level install has no hashable executable (missing Info.plist, CFBundleExecutable, or binary): ${user_bundle}" >&2
            failed=1
        elif ! verify_codesign "$user_bundle"; then
            echo "ERROR: ${name} user-level install failed codesign verification" >&2
            failed=1
        fi
    fi

    if [ -d "$system_bundle" ]; then
        if ! system_hash="$(hash_bundle_executable "$system_bundle" 2>/dev/null)"; then
            echo "ERROR: ${name} system-level install has no hashable executable (missing Info.plist, CFBundleExecutable, or binary): ${system_bundle}" >&2
            failed=1
        elif ! verify_codesign "$system_bundle"; then
            echo "ERROR: ${name} system-level install failed codesign verification" >&2
            failed=1
        fi
    fi

    if [ -n "$user_hash" ] && [ -n "$system_hash" ] && [ "$user_hash" != "$system_hash" ]; then
        echo "ERROR: stale duplicate ${name} install detected." >&2
        echo "       User and system plugin binaries differ, so a DAW may load a stale copy." >&2
        echo "       Remove the stale ${format_arg} copy; the system scope belongs to the" >&2
        echo "       official installer package only." >&2
        failed=1
    fi

    if [ -n "$EXPECT_BUNDLE" ] && [ -d "$user_bundle" ]; then
        check_expected_source "$name" "$user_bundle" "$EXPECT_BUNDLE" || failed=1
    fi

    if [ "$failed" -eq 0 ]; then
        echo "${name} install verification passed."
    fi

    return "$failed"
}

overall_status=0
case "$FORMAT" in
    all)
        check_format "VST3" "vst3" "$USER_VST3_PATH" "$SYSTEM_VST3_PATH" || overall_status=1
        check_format "AU" "au" "$USER_AU_PATH" "$SYSTEM_AU_PATH" || overall_status=1
        ;;
    vst3)
        check_format "VST3" "vst3" "$USER_VST3_PATH" "$SYSTEM_VST3_PATH" || overall_status=1
        ;;
    au)
        check_format "AU" "au" "$USER_AU_PATH" "$SYSTEM_AU_PATH" || overall_status=1
        ;;
esac

if [ "$overall_status" -eq 0 ]; then
    echo "macOS plugin install verification passed."
fi

exit "$overall_status"
