#!/bin/bash
# Installs built Release VST3/AU bundles into the user plug-in folders.
#
# This is the explicit development install step; scripts/build.sh is
# build-only and never installs. Guarantees:
#   - User scope only: destinations are the user plug-in folders and the
#     script contains no privilege escalation, so it cannot write anywhere
#     the invoking user cannot.
#   - No signing: the Xcode-generated targets already ad-hoc sign the built
#     artifact, and ditto preserves that signature. The install is proven to
#     be an exact copy by the readback step instead of being re-signed.
#   - Duplicate rejection: refuses to install while a system-scope copy of
#     the same bundle exists, because a DAW could then load either copy.
#   - Readback: after copying, the installed bundle must match the source
#     artifact exactly (executable hash plus Info.plist identity) and pass
#     codesign verification.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
# shellcheck source=lib/macos_plugin_paths.sh
source "${SCRIPT_DIR}/lib/macos_plugin_paths.sh"

FORMAT="all"
DRY_RUN=0
ALLOW_SYSTEM_DUPLICATE=0
SKIP_CODESIGN="${SPATIAL_ECHO_SKIP_CODESIGN:-0}"
ARTIFACT_ROOT="${SPATIAL_ECHO_RELEASE_ARTIFACT_ROOT:-${ROOT_DIR}/build/cmake/macos-universal-release/SpatialEcho_artefacts/Release}"

usage() {
    cat <<USAGE
Usage: $0 [--format all|vst3|au] [--dry-run] [--allow-system-duplicate]
          [--skip-codesign]

Copies the built Release bundles from
  ${ARTIFACT_ROOT}
into the user plug-in folders, then verifies the installed copy matches the
source artifact exactly (hash and identity readback).

Build the artifacts first: ./scripts/build.sh release vst3|au|all

--dry-run                 print the install plan without writing anything
--allow-system-duplicate  install even though a system-scope copy exists;
                          verification still fails until the copies match
--skip-codesign           skip codesign checks during readback
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
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --allow-system-duplicate)
            ALLOW_SYSTEM_DUPLICATE=1
            shift
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

install_format() {
    local name="$1"
    local format_arg="$2"
    local source="$3"
    local user_dest="$4"
    local system_dest="$5"

    if [ ! -d "$source" ]; then
        echo "Error: ${name} Release artifact is missing: ${source}" >&2
        echo "       Build it first: ./scripts/build.sh release ${format_arg}" >&2
        exit 1
    fi

    if [ -e "$system_dest" ] && [ "$ALLOW_SYSTEM_DUPLICATE" -eq 0 ]; then
        echo "Error: a system-scope ${name} copy exists: ${system_dest}" >&2
        echo "       Development installs are user-only, and a second copy of the" >&2
        echo "       same bundle means a DAW may load either one." >&2
        echo "       Remove the system copy, or pass --allow-system-duplicate and" >&2
        echo "       reconcile afterwards (verification fails until the copies match)." >&2
        exit 1
    fi

    echo ""
    echo "Installing Release ${name}:"
    echo "  from: ${source}"
    echo "  to:   ${user_dest}"

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "  (dry run: nothing written)"
        return 0
    fi

    mkdir -p "$(dirname "$user_dest")"
    rm -rf "$user_dest"
    ditto "$source" "$user_dest"

    local verify_args=(--format "$format_arg" --expect-bundle "$source")
    if [ "$SKIP_CODESIGN" -eq 1 ]; then
        verify_args+=(--skip-codesign)
    fi

    echo ""
    echo "Reading back ${name} install..."
    "${SCRIPT_DIR}/verify_macos_plugin_installs.sh" "${verify_args[@]}"
}

case "$FORMAT" in
    all)
        install_format "VST3" "vst3" "${ARTIFACT_ROOT}/VST3/${SPATIAL_ECHO_BUNDLE_BASENAME}.vst3" "$USER_VST3_PATH" "$SYSTEM_VST3_PATH"
        install_format "AU" "au" "${ARTIFACT_ROOT}/AU/${SPATIAL_ECHO_BUNDLE_BASENAME}.component" "$USER_AU_PATH" "$SYSTEM_AU_PATH"
        ;;
    vst3)
        install_format "VST3" "vst3" "${ARTIFACT_ROOT}/VST3/${SPATIAL_ECHO_BUNDLE_BASENAME}.vst3" "$USER_VST3_PATH" "$SYSTEM_VST3_PATH"
        ;;
    au)
        install_format "AU" "au" "${ARTIFACT_ROOT}/AU/${SPATIAL_ECHO_BUNDLE_BASENAME}.component" "$USER_AU_PATH" "$SYSTEM_AU_PATH"
        ;;
esac

if [ "$DRY_RUN" -eq 1 ]; then
    echo ""
    echo "Dry run complete; no files were written."
else
    echo ""
    echo "User-scope plugin install complete."
fi
