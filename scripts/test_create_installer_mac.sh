#!/bin/bash
# Builds the macOS installer from temporary fixture bundles and verifies that
# package components replace installed bundles even when those bundles report
# a higher version number, and reject incomplete or incompatible artifacts.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

make_bundle() {
    local bundle="$1"
    local package_type="$2"

    mkdir -p "${bundle}/Contents/MacOS"
    cp "${TMP_DIR}/universal-fixture" "${bundle}/Contents/MacOS/Spatial-Echo"
    mkdir -p "${bundle}/Contents/Resources/Licenses"
    cp "${fixture_root}/docs/licenses/"* "${bundle}/Contents/Resources/Licenses/"
    cat > "${bundle}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>Spatial-Echo</string>
    <key>CFBundleIdentifier</key>
    <string>com.echoeslab.spatialecho</string>
    <key>CFBundlePackageType</key>
    <string>${package_type}</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1.0.0</string>
    <key>AudioComponents</key>
    <array><dict>
        <key>type</key><string>aufx</string>
        <key>subtype</key><string>SpEc</string>
        <key>manufacturer</key><string>EchL</string>
    </dict></array>
</dict>
</plist>
PLIST
}

printf 'int main(void) { return 0; }\n' > "${TMP_DIR}/fixture.c"
xcrun clang -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
    "${TMP_DIR}/fixture.c" -o "${TMP_DIR}/universal-fixture"

fixture_root="${TMP_DIR}/repo"
mkdir -p \
    "${fixture_root}/scripts" \
    "${fixture_root}/docs/licenses" \
    "${fixture_root}/installer/mac_resources" \
    "${fixture_root}/build/cmake/macos-universal-release/package_stage/VST3" \
    "${fixture_root}/build/cmake/macos-universal-release/package_stage/AU" \
    "${fixture_root}/build/cmake/macos-universal-release/package_stage/Standalone"

cp "${ROOT_DIR}/scripts/create_installer_mac.sh" "${fixture_root}/scripts/"
cp "${ROOT_DIR}/installer/Distribution.xml" "${fixture_root}/installer/"
cp "${ROOT_DIR}/installer/entitlements.plist" "${fixture_root}/installer/"
cp "${ROOT_DIR}/installer/mac_resources/"* "${fixture_root}/installer/mac_resources/"
cp "${ROOT_DIR}/docs/licenses/"* "${fixture_root}/docs/licenses/"

cat > "${fixture_root}/CMakeLists.txt" <<'CMAKE'
project(SpatialEcho VERSION 1.0.0 LANGUAGES CXX)
CMAKE

make_bundle \
    "${fixture_root}/build/cmake/macos-universal-release/package_stage/VST3/Spatial-Echo.vst3" \
    "BNDL"
make_bundle \
    "${fixture_root}/build/cmake/macos-universal-release/package_stage/AU/Spatial-Echo.component" \
    "BNDL"
make_bundle \
    "${fixture_root}/build/cmake/macos-universal-release/package_stage/Standalone/Spatial-Echo.app" \
    "APPL"

(
    cd "${TMP_DIR}"
    "${fixture_root}/scripts/create_installer_mac.sh" --skip-build --skip-sign > "${TMP_DIR}/build.log"
)

package="${fixture_root}/build/release_packages/Spatial-Echo-v1.0.0-mac.pkg"
[ -f "${package}" ] || fail "installer fixture build did not create the package"

expanded="${TMP_DIR}/expanded"
pkgutil --expand-full "${package}" "${expanded}"

minimum_os="$(xmllint --xpath 'string(/installer-gui-script/volume-check/allowed-os-versions/os-version/@min)' "${expanded}/Distribution")"
[ "$minimum_os" = "11.0" ] || fail "installer does not enforce the documented minimum macOS version"

for component in Spatial-Echo-VST3.pkg Spatial-Echo-AU.pkg Spatial-Echo-Standalone.pkg; do
    package_info="${expanded}/${component}/PackageInfo"
    [ -f "${package_info}" ] || fail "missing PackageInfo for ${component}"

    version_checked_count="$(xmllint --xpath 'count(/pkg-info/bundle-version/bundle)' "${package_info}")"
    [ "${version_checked_count}" = "0" ] || \
        fail "${component} would skip an installed bundle that reports a higher version"

    upgrade_count="$(xmllint --xpath 'count(/pkg-info/upgrade-bundle/bundle)' "${package_info}")"
    [ "${upgrade_count}" = "1" ] || \
        fail "${component} is not configured for atomic bundle replacement"

    case "$component" in
        Spatial-Echo-VST3.pkg) bundle_name=Spatial-Echo.vst3 ;;
        Spatial-Echo-AU.pkg) bundle_name=Spatial-Echo.component ;;
        Spatial-Echo-Standalone.pkg) bundle_name=Spatial-Echo.app ;;
    esac
    resources="${expanded}/${component}/Payload/${bundle_name}/Contents/Resources"
    for resource in APACHE-2.0.txt HUTUBS.md THIRD_PARTY_NOTICES.md VST3_SDK_MIT_LICENSE.txt; do
        cmp "${fixture_root}/docs/licenses/${resource}" "${resources}/Licenses/${resource}" \
            || fail "${component} is missing the current ${resource}"
    done
done

expect_rejected() {
    local message="$1"
    shift
    if "${fixture_root}/scripts/create_installer_mac.sh" --skip-build --skip-sign "$@" > "${TMP_DIR}/reject.log" 2>&1; then
        fail "invalid input was accepted: $message"
    fi
    grep -Fq "$message" "${TMP_DIR}/reject.log" || { cat "${TMP_DIR}/reject.log"; fail "expected error: $message"; }
    cmp "$package" "${TMP_DIR}/original.pkg" || fail "rejected input changed the existing package"
}

cp "$package" "${TMP_DIR}/original.pkg"
stage="${fixture_root}/build/cmake/macos-universal-release/package_stage"
binary="${stage}/VST3/Spatial-Echo.vst3/Contents/MacOS/Spatial-Echo"

mv "$binary" "${TMP_DIR}/saved-binary"
expect_rejected "Missing or non-executable bundle binary"
printf '#!/bin/sh\nexit 0\n' > "$binary"
chmod +x "$binary"
expect_rejected "Not a Mach-O binary"
lipo "${TMP_DIR}/saved-binary" -thin arm64 -output "$binary"
expect_rejected "Missing x86_64 architecture"
cp "${TMP_DIR}/saved-binary" "$binary"

notice="${stage}/VST3/Spatial-Echo.vst3/Contents/Resources/Licenses/HUTUBS.md"
mv "$notice" "${TMP_DIR}/bundle-HUTUBS.md"
expect_rejected "Missing or stale bundle resource"
printf 'obsolete notice\n' > "$notice"
expect_rejected "Missing or stale bundle resource"
mv "${TMP_DIR}/bundle-HUTUBS.md" "$notice"

plist="${stage}/AU/Spatial-Echo.component/Contents/Info.plist"
plutil -replace AudioComponents.0.subtype -string Sev2 "$plist"
expect_rejected "Unexpected AU subtype"
plutil -replace AudioComponents.0.subtype -string SpEc "$plist"
plutil -replace CFBundleIdentifier -string com.example.other "$plist"
expect_rejected "Unexpected bundle identity"
plutil -replace CFBundleIdentifier -string com.echoeslab.spatialecho "$plist"
plutil -replace CFBundleShortVersionString -string 1.0.1 "$plist"
expect_rejected "Unexpected CFBundleShortVersionString"
plutil -replace CFBundleShortVersionString -string 1.0.0 "$plist"

mv "${fixture_root}/docs/licenses/HUTUBS.md" "${TMP_DIR}/HUTUBS.md"
expect_rejected "Missing or empty required resource"
mv "${TMP_DIR}/HUTUBS.md" "${fixture_root}/docs/licenses/HUTUBS.md"
expect_rejected "Cannot notarize an unsigned installer" --notarize

echo "macOS installer replacement, resource, and rejection tests passed"
