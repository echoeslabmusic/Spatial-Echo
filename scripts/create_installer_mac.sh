#!/bin/bash
# ---------------------------------------------------------------------------
# create_installer_mac.sh: build and package the Spatial Echo macOS installer.
#
# Builds VST3, AU, and Standalone as Universal Binaries through the tracked
# CMake preset, code-signs staged artifacts, assembles a distribution .pkg via
# pkgbuild/productbuild, and optionally notarizes and staples the installer.
#
# Usage:
#   ./scripts/create_installer_mac.sh
#   ./scripts/create_installer_mac.sh --skip-build
#   ./scripts/create_installer_mac.sh --skip-build --skip-sign
#   ./scripts/create_installer_mac.sh --notarize
#
# Environment variables (override defaults):
#   SIGN_APP        Developer ID Application identity (code signing)
#   SIGN_PKG        Developer ID Installer identity (pkg signing)
#   NOTARY_PROFILE  Keychain profile name for notarytool (default: AC_PASSWORD)
#   JUCE_DIR        JUCE source checkout used by CMake (default: ~/JUCE)
# ---------------------------------------------------------------------------

set -euo pipefail

# Parse flags
SKIP_BUILD=false
SKIP_SIGN=false
DO_NOTARIZE=false

for arg in "$@"; do
    case "$arg" in
        --skip-build)  SKIP_BUILD=true ;;
        --skip-sign)   SKIP_SIGN=true ;;
        --notarize)    DO_NOTARIZE=true ;;
        --help|-h)
            echo "Usage: $0 [--skip-build] [--skip-sign] [--notarize]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            exit 1
            ;;
    esac
done

# Resolve paths
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
# Preset lookup is relative to the current directory, including when this
# script is called through an absolute path from another directory.
cd "$ROOT_DIR"
INSTALLER_DIR="${ROOT_DIR}/installer"
ENTITLEMENTS="${INSTALLER_DIR}/entitlements.plist"
DIST_TEMPLATE="${INSTALLER_DIR}/Distribution.xml"
RESOURCES_DIR="${INSTALLER_DIR}/mac_resources"

STAGING_DIR="${ROOT_DIR}/build/mac_staging"
OUTPUT_DIR="${ROOT_DIR}/build/release_packages"
CMAKE_PRESET="macos-universal-release"
CMAKE_BUILD_DIR="${ROOT_DIR}/build/cmake/${CMAKE_PRESET}"
CMAKE_STAGE_DIR="${CMAKE_BUILD_DIR}/package_stage"

NOTARY_PROFILE="${NOTARY_PROFILE:-AC_PASSWORD}"

# Signing identities: set them in the environment, or pass --skip-sign.
SIGN_APP="${SIGN_APP:-}"
SIGN_PKG="${SIGN_PKG:-}"

# License files required in every staged bundle
LICENSE_FILES=(
    "${ROOT_DIR}/docs/licenses/APACHE-2.0.txt"
    "${ROOT_DIR}/docs/licenses/HUTUBS.md"
    "${ROOT_DIR}/docs/licenses/THIRD_PARTY_NOTICES.md"
    "${ROOT_DIR}/docs/licenses/VST3_SDK_MIT_LICENSE.txt"
)

# Helpers
info()  { echo "==> $*"; }
ok()    { echo "    ok: $*"; }
fail()  { echo "error: $*" >&2; exit 1; }

# Read version from the CMake project
VERSION=$(sed -n 's/^project(SpatialEcho VERSION \([^ ]*\).*/\1/p' "${ROOT_DIR}/CMakeLists.txt" | head -1)
if [ -z "$VERSION" ]; then
    fail "Could not extract version from CMakeLists.txt"
fi
info "Version: $VERSION"

# Validate prerequisites
command -v cmake >/dev/null 2>&1 || fail "cmake not found"
command -v pkgbuild   >/dev/null 2>&1 || fail "pkgbuild not found"
command -v productbuild >/dev/null 2>&1 || fail "productbuild not found"
command -v lipo >/dev/null 2>&1 || fail "lipo not found"

if [ "$DO_NOTARIZE" = true ] && [ "$SKIP_SIGN" = true ]; then
    fail "Cannot notarize an unsigned installer; remove --skip-sign"
fi

for resource in "${LICENSE_FILES[@]}"; do
    [ -s "$resource" ] || fail "Missing or empty required resource: $resource"
done

if [ "$SKIP_SIGN" = false ]; then
    if [ -z "$SIGN_APP" ] || [ -z "$SIGN_PKG" ]; then
        echo ""
        echo "Code signing identities not set. Provide them via environment variables:"
        echo "  export SIGN_APP=\"Developer ID Application: Echoes Lab (XXXXXXXXXX)\""
        echo "  export SIGN_PKG=\"Developer ID Installer: Echoes Lab (XXXXXXXXXX)\""
        echo ""
        echo "Available Developer ID identities on this machine:"
        security find-identity -v -p basic 2>/dev/null | grep "Developer ID" || echo "  (none found)"
        echo ""
        echo "To build without signing, re-run with --skip-sign"
        exit 1
    fi
fi

# Build and stage Universal Binaries
if [ "$SKIP_BUILD" = false ]; then
    info "Configuring CMake preset: ${CMAKE_PRESET}"
    cmake --preset "$CMAKE_PRESET"

    info "Building and staging Release artifacts"
    cmake --build --preset "$CMAKE_PRESET" --target SpatialEchoStageReleaseArtifacts
    ok "Release artifacts staged"
else
    info "Skipping CMake build (--skip-build)"
fi

# Locate and stage build artifacts
BUILT_VST3_BUNDLE="${CMAKE_STAGE_DIR}/VST3/Spatial-Echo.vst3"
BUILT_AU_BUNDLE="${CMAKE_STAGE_DIR}/AU/Spatial-Echo.component"
BUILT_APP_BUNDLE="${CMAKE_STAGE_DIR}/Standalone/Spatial-Echo.app"

PACKAGE_STAGE="${ROOT_DIR}/build/mac_package_stage"
VST3_BUNDLE="${PACKAGE_STAGE}/VST3/Spatial-Echo.vst3"
AU_BUNDLE="${PACKAGE_STAGE}/AU/Spatial-Echo.component"
APP_BUNDLE="${PACKAGE_STAGE}/Standalone/Spatial-Echo.app"

validate_bundle() {
    local bundle="$1"
    local info_plist="${bundle}/Contents/Info.plist"
    local executable identifier version version_key binary archs resource category bundled_resource

    [ -d "$bundle" ] || fail "Missing build artifact: $bundle"
    [ -f "$info_plist" ] || fail "Missing Info.plist: $bundle"
    executable="$(/usr/bin/plutil -extract CFBundleExecutable raw -o - "$info_plist")" \
        || fail "Missing CFBundleExecutable: $bundle"
    case "$executable" in
        ""|.|..|*/*) fail "Invalid CFBundleExecutable: $bundle" ;;
    esac
    identifier="$(/usr/bin/plutil -extract CFBundleIdentifier raw -o - "$info_plist")" \
        || fail "Missing CFBundleIdentifier: $bundle"
    [ "$identifier" = "com.echoeslab.spatialecho" ] \
        || fail "Unexpected bundle identity '$identifier': $bundle"
    for version_key in CFBundleShortVersionString CFBundleVersion; do
        version="$(/usr/bin/plutil -extract "$version_key" raw -o - "$info_plist")" \
            || fail "Missing $version_key: $bundle"
        [ "$version" = "$VERSION" ] \
            || fail "Unexpected $version_key '$version' (expected $VERSION): $bundle"
    done
    binary="${bundle}/Contents/MacOS/${executable}"
    [ -f "$binary" ] && [ -x "$binary" ] || fail "Missing or non-executable bundle binary: $binary"
    archs="$(lipo -archs "$binary" 2>/dev/null)" || fail "Not a Mach-O binary: $binary"
    case " $archs " in *" arm64 "*) ;; *) fail "Missing arm64 architecture: $binary ($archs)" ;; esac
    case " $archs " in *" x86_64 "*) ;; *) fail "Missing x86_64 architecture: $binary ($archs)" ;; esac
    for resource in "${LICENSE_FILES[@]}"; do
        bundled_resource="${bundle}/Contents/Resources/Licenses/$(basename "$resource")"
        cmp -s "$resource" "$bundled_resource" \
            || fail "Missing or stale bundle resource; rebuild and stage again: $bundled_resource"
    done
    info "  $(basename "$bundle") -> $archs; $identifier $version"
}

for artifact in "$BUILT_VST3_BUNDLE" "$BUILT_AU_BUNDLE" "$BUILT_APP_BUNDLE"; do
    validate_bundle "$artifact"
done
for au_field in type subtype manufacturer; do
    case "$au_field" in type) expected=aufx ;; subtype) expected=SpEc ;; manufacturer) expected=EchL ;; esac
    actual="$(/usr/bin/plutil -extract "AudioComponents.0.${au_field}" raw -o - "$BUILT_AU_BUNDLE/Contents/Info.plist")" \
        || fail "Missing AU $au_field"
    [ "$actual" = "$expected" ] || fail "Unexpected AU $au_field '$actual' (expected $expected)"
done

rm -rf "$PACKAGE_STAGE"
mkdir -p "${PACKAGE_STAGE}/VST3" "${PACKAGE_STAGE}/AU" "${PACKAGE_STAGE}/Standalone"
cp -R "$BUILT_VST3_BUNDLE" "${PACKAGE_STAGE}/VST3/"
cp -R "$BUILT_AU_BUNDLE" "${PACKAGE_STAGE}/AU/"
cp -R "$BUILT_APP_BUNDLE" "${PACKAGE_STAGE}/Standalone/"

# CMake already populated and signed these bundles. Resource validation above
# keeps plug-in-only installs complete without changing any sealed resources
# when --skip-sign preserves build-local ad-hoc signatures.

# Code sign
codesign_bundle() {
    local bundle="$1"
    local label="$2"

    if [ "$SKIP_SIGN" = true ]; then
        info "Skipping code sign: $label"
        return
    fi

    info "Signing: $label"
    codesign --force --deep --strict \
             --sign "$SIGN_APP" \
             --options runtime \
             --entitlements "$ENTITLEMENTS" \
             --timestamp \
             "$bundle"

    codesign --verify --deep --strict "$bundle"
    ok "Signed: $label"
}

codesign_bundle "$VST3_BUNDLE" "VST3"
codesign_bundle "$AU_BUNDLE"   "AU"
codesign_bundle "$APP_BUNDLE"  "Standalone"

# Create staging directory & component packages
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR" "$OUTPUT_DIR"

pkgbuild_component() {
    local component="$1"
    local install_location="$2"
    local identifier="$3"
    local output="$4"
    local component_name
    local component_root
    local component_policy

    component_name="$(basename "$component")"
    component_root="${STAGING_DIR}/component-roots/${identifier}"
    component_policy="${STAGING_DIR}/$(basename "${output%.pkg}")-components.plist"

    rm -rf "$component_root"
    mkdir -p "$component_root"
    ditto "$component" "${component_root}/${component_name}"

    pkgbuild --analyze --root "$component_root" "$component_policy" >/dev/null
    # Replace an installed bundle even when it reports a higher version.
    plutil -replace 0.BundleIsVersionChecked -bool false "$component_policy"
    plutil -replace 0.BundleOverwriteAction -string upgrade "$component_policy"
    # pkgbuild marks bundles relocatable, which lets Installer write the app
    # over any other copy with the same bundle ID (for example in
    # ~/Applications) instead of into /Applications.
    plutil -replace 0.BundleIsRelocatable -bool false "$component_policy"

    info "Packaging: ${component_name} -> $install_location"
    if [ "$SKIP_SIGN" = false ]; then
        pkgbuild \
            --root "$component_root" \
            --component-plist "$component_policy" \
            --install-location "$install_location" \
            --identifier "$identifier" \
            --version "$VERSION" \
            --sign "$SIGN_PKG" \
            "$output"
    else
        pkgbuild \
            --root "$component_root" \
            --component-plist "$component_policy" \
            --install-location "$install_location" \
            --identifier "$identifier" \
            --version "$VERSION" \
            "$output"
    fi
}

pkgbuild_component "$VST3_BUNDLE" \
    "/Library/Audio/Plug-Ins/VST3" \
    "com.echoeslab.spatialecho.vst3" \
    "${STAGING_DIR}/Spatial-Echo-VST3.pkg"

pkgbuild_component "$AU_BUNDLE" \
    "/Library/Audio/Plug-Ins/Components" \
    "com.echoeslab.spatialecho.au" \
    "${STAGING_DIR}/Spatial-Echo-AU.pkg"

pkgbuild_component "$APP_BUNDLE" \
    "/Applications" \
    "com.echoeslab.spatialecho.standalone" \
    "${STAGING_DIR}/Spatial-Echo-Standalone.pkg"

ok "Component packages created"

# Patch version into Distribution.xml and build distribution
DIST_PATCHED="${STAGING_DIR}/Distribution.xml"
sed "s/VERSION_PLACEHOLDER/${VERSION}/g" "$DIST_TEMPLATE" > "$DIST_PATCHED"

INSTALLER_NAME="Spatial-Echo-v${VERSION}-mac.pkg"
INSTALLER_PATH="${OUTPUT_DIR}/${INSTALLER_NAME}"
DELIVERY_SUFFIX=""
if [ "$SKIP_SIGN" = true ]; then
    DELIVERY_SUFFIX="-unsigned"
fi
DELIVERY_NAME="Spatial-Echo-v${VERSION}-mac${DELIVERY_SUFFIX}"
DELIVERY_DIR="${OUTPUT_DIR}/${DELIVERY_NAME}"
DELIVERY_ARCHIVE="${OUTPUT_DIR}/${DELIVERY_NAME}.zip"

info "Building distribution installer..."
if [ "$SKIP_SIGN" = false ]; then
    productbuild \
        --distribution "$DIST_PATCHED" \
        --package-path "$STAGING_DIR" \
        --resources "$RESOURCES_DIR" \
        --sign "$SIGN_PKG" \
        "$INSTALLER_PATH"
else
    productbuild \
        --distribution "$DIST_PATCHED" \
        --package-path "$STAGING_DIR" \
        --resources "$RESOURCES_DIR" \
        "$INSTALLER_PATH"
fi

ok "Installer created: $INSTALLER_PATH"

# Notarize (optional)
if [ "$DO_NOTARIZE" = true ]; then
    if [ "$SKIP_SIGN" = true ]; then
        fail "Cannot notarize an unsigned installer; remove --skip-sign"
    fi

    info "Submitting for notarization..."
    xcrun notarytool submit "$INSTALLER_PATH" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait

    info "Stapling notarization ticket..."
    xcrun stapler staple "$INSTALLER_PATH"

    ok "Notarization complete"
fi

# Generate SHA-256 checksum
HASH=$(shasum -a 256 "$INSTALLER_PATH" | awk '{print $1}')
echo "$HASH  $INSTALLER_NAME" > "${INSTALLER_PATH}.sha256"
ok "SHA-256: $HASH"

# Unsigned builds: zip the installer with its checksum and README
if [ "$SKIP_SIGN" = true ]; then
    rm -rf "$DELIVERY_DIR" "$DELIVERY_ARCHIVE"
    mkdir -p "$DELIVERY_DIR"
    cp "$INSTALLER_PATH" "${DELIVERY_DIR}/${INSTALLER_NAME}"
    cp "${INSTALLER_PATH}.sha256" "${DELIVERY_DIR}/${INSTALLER_NAME}.sha256"
    cp "${RESOURCES_DIR}/README.txt" "${DELIVERY_DIR}/README.txt"
    (
        cd "$OUTPUT_DIR"
        /usr/bin/zip -r -X "$(basename "$DELIVERY_ARCHIVE")" "$DELIVERY_NAME" >/dev/null
    )
    ok "Delivery archive: $DELIVERY_ARCHIVE"
fi

# Summary
echo ""
echo "============================================"
echo " Spatial Echo macOS installer complete"
echo "============================================"
echo " Version:    $VERSION"
echo " Installer:  $INSTALLER_PATH"
echo " SHA-256:    ${INSTALLER_PATH}.sha256"
if [ "$SKIP_SIGN" = true ]; then
    echo " Delivery:   $DELIVERY_ARCHIVE"
fi
echo " Signed:     $([ "$SKIP_SIGN" = false ] && echo "yes" || echo "no")"
echo " Notarized:  $([ "$DO_NOTARIZE" = true ] && echo "yes" || echo "no")"
echo ""
echo " Install locations:"
echo "   VST3:       /Library/Audio/Plug-Ins/VST3/Spatial-Echo.vst3"
echo "   AU:         /Library/Audio/Plug-Ins/Components/Spatial-Echo.component"
echo "   Standalone: /Applications/Spatial-Echo.app"
echo "   Presets:    ~/Library/Application Support/Spatial Echo/Presets/"
echo "============================================"
