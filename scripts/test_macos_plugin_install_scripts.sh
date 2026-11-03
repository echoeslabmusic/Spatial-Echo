#!/bin/bash
# Tests macOS plugin install, sync, and verification helpers with temporary
# bundles (temp plugin roots only; never touches real plug-in folders).

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

fail() {
    echo "FAIL: $1"
    shift
    for log in "$@"; do
        [ -f "$log" ] && cat "$log"
    done
    exit 1
}

# make_bundle <bundle-dir> <payload> [executable-name]
# Creates a minimal plugin bundle with an Info.plist so executables are
# derived from CFBundleExecutable exactly like real installs.
make_bundle() {
    local bundle="$1"
    local payload="$2"
    local executable="${3:-$(basename "${bundle%.*}")}"

    mkdir -p "${bundle}/Contents/MacOS"
    printf '%s' "${payload}" > "${bundle}/Contents/MacOS/${executable}"
    chmod +x "${bundle}/Contents/MacOS/${executable}"
    cat > "${bundle}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${executable}</string>
    <key>CFBundleIdentifier</key>
    <string>com.echoeslab.spatialecho</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
</dict>
</plist>
PLIST
}

run_with_fake_paths() {
    SPATIAL_ECHO_USER_VST3_PATH="${TMP_DIR}/user/Spatial-Echo.vst3" \
    SPATIAL_ECHO_SYSTEM_VST3_PATH="${TMP_DIR}/system/Spatial-Echo.vst3" \
    SPATIAL_ECHO_USER_AU_PATH="${TMP_DIR}/user/Spatial-Echo.component" \
    SPATIAL_ECHO_SYSTEM_AU_PATH="${TMP_DIR}/system/Spatial-Echo.component" \
    SPATIAL_ECHO_RELEASE_ARTIFACT_ROOT="${TMP_DIR}/artifacts" \
    "$@"
}

hash_file() {
    shasum -a 256 "$1" | awk '{print $1}'
}

echo "Case 1: stale user/system duplicate fails verification"
make_bundle "${TMP_DIR}/user/Spatial-Echo.vst3" "fresh-vst3"
make_bundle "${TMP_DIR}/system/Spatial-Echo.vst3" "stale-vst3"

set +e
run_with_fake_paths "${SCRIPT_DIR}/verify_macos_plugin_installs.sh" --format vst3 --skip-codesign > "${TMP_DIR}/verify-fail.log" 2>&1
verify_status=$?
set -e

[ "${verify_status}" -ne 0 ] || fail "expected stale duplicate verification to fail" "${TMP_DIR}/verify-fail.log"
grep -qi "stale duplicate" "${TMP_DIR}/verify-fail.log" || fail "expected stale duplicate failure message" "${TMP_DIR}/verify-fail.log"

echo "Case 2: matching user/system copies pass verification"
rm -rf "${TMP_DIR}/system/Spatial-Echo.vst3"
ditto "${TMP_DIR}/user/Spatial-Echo.vst3" "${TMP_DIR}/system/Spatial-Echo.vst3"

run_with_fake_paths "${SCRIPT_DIR}/verify_macos_plugin_installs.sh" --format vst3 --skip-codesign > "${TMP_DIR}/verify-pass.log"
grep -q "install verification passed" "${TMP_DIR}/verify-pass.log" || fail "expected matching duplicate to pass" "${TMP_DIR}/verify-pass.log"

echo "Case 3: executables are derived from CFBundleExecutable, not a fixed name"
make_bundle "${TMP_DIR}/user/Spatial-Echo.component" "custom-au" "Custom-Exec"
run_with_fake_paths "${SCRIPT_DIR}/verify_macos_plugin_installs.sh" --format au --skip-codesign > "${TMP_DIR}/verify-custom.log"
grep -q "executable=Custom-Exec" "${TMP_DIR}/verify-custom.log" || fail "expected derived executable name in verification output" "${TMP_DIR}/verify-custom.log"
rm -rf "${TMP_DIR}/user/Spatial-Echo.component"

echo "Case 4: installer dry run writes nothing"
rm -rf "${TMP_DIR}/user" "${TMP_DIR}/system" "${TMP_DIR}/artifacts"
make_bundle "${TMP_DIR}/artifacts/VST3/Spatial-Echo.vst3" "artifact-vst3"

run_with_fake_paths "${SCRIPT_DIR}/install_macos_plugins.sh" --format vst3 --dry-run --skip-codesign > "${TMP_DIR}/install-dry.log"
[ ! -e "${TMP_DIR}/user/Spatial-Echo.vst3" ] || fail "expected dry run to write nothing" "${TMP_DIR}/install-dry.log"
grep -qi "dry run" "${TMP_DIR}/install-dry.log" || fail "expected dry run notice" "${TMP_DIR}/install-dry.log"

echo "Case 5: installer performs user-scope install with exact readback"
run_with_fake_paths "${SCRIPT_DIR}/install_macos_plugins.sh" --format vst3 --skip-codesign > "${TMP_DIR}/install.log"

source_hash="$(hash_file "${TMP_DIR}/artifacts/VST3/Spatial-Echo.vst3/Contents/MacOS/Spatial-Echo")"
installed_hash="$(hash_file "${TMP_DIR}/user/Spatial-Echo.vst3/Contents/MacOS/Spatial-Echo")"
[ "${source_hash}" = "${installed_hash}" ] || fail "expected installed bundle to match the source artifact" "${TMP_DIR}/install.log"
grep -q "matches the source bundle" "${TMP_DIR}/install.log" || fail "expected hash/identity readback confirmation" "${TMP_DIR}/install.log"
[ ! -e "${TMP_DIR}/system/Spatial-Echo.vst3" ] || fail "expected installer to leave system scope untouched" "${TMP_DIR}/install.log"

echo "Case 6: readback fails when the installed copy is tampered with"
printf 'tampered' > "${TMP_DIR}/user/Spatial-Echo.vst3/Contents/MacOS/Spatial-Echo"

set +e
run_with_fake_paths "${SCRIPT_DIR}/verify_macos_plugin_installs.sh" --format vst3 --skip-codesign \
    --expect-bundle "${TMP_DIR}/artifacts/VST3/Spatial-Echo.vst3" > "${TMP_DIR}/readback-fail.log" 2>&1
readback_status=$?
set -e

[ "${readback_status}" -ne 0 ] || fail "expected tampered readback to fail" "${TMP_DIR}/readback-fail.log"
grep -qi "does not match the source bundle" "${TMP_DIR}/readback-fail.log" || fail "expected readback mismatch message" "${TMP_DIR}/readback-fail.log"

echo "Case 7: installer rejects an existing system-scope duplicate"
rm -rf "${TMP_DIR}/user/Spatial-Echo.vst3"
make_bundle "${TMP_DIR}/system/Spatial-Echo.vst3" "system-vst3"

set +e
run_with_fake_paths "${SCRIPT_DIR}/install_macos_plugins.sh" --format vst3 --skip-codesign > "${TMP_DIR}/install-dup.log" 2>&1
dup_status=$?
set -e

[ "${dup_status}" -ne 0 ] || fail "expected installer to reject a system-scope duplicate" "${TMP_DIR}/install-dup.log"
grep -qi "system-scope" "${TMP_DIR}/install-dup.log" || fail "expected system-scope duplicate message" "${TMP_DIR}/install-dup.log"
[ ! -e "${TMP_DIR}/user/Spatial-Echo.vst3" ] || fail "expected rejection before any write" "${TMP_DIR}/install-dup.log"

echo "Case 8: installer contains no privilege escalation"
if grep -nE 'osascript|sudo|with administrator privileges' "${SCRIPT_DIR}/install_macos_plugins.sh" > "${TMP_DIR}/escalation.log"; then
    fail "expected installer to contain no privilege escalation" "${TMP_DIR}/escalation.log"
fi

echo "macOS plugin install scripts test passed"
