#!/bin/bash
# Shared macOS plugin install identity and path defaults for Spatial Echo
# scripts. Single source of truth for the installed bundle basename: change
# SPATIAL_ECHO_BUNDLE_BASENAME here (or via env) when the product identity
# changes, and every install/sync/verify script follows.
#
# Sourced, not executed. Callers must set nothing in advance; every value
# honors the same SPATIAL_ECHO_* env overrides the scripts documented before.

SPATIAL_ECHO_BUNDLE_BASENAME="${SPATIAL_ECHO_BUNDLE_BASENAME:-Spatial-Echo}"

USER_VST3_PATH="${SPATIAL_ECHO_USER_VST3_PATH:-${HOME}/Library/Audio/Plug-Ins/VST3/${SPATIAL_ECHO_BUNDLE_BASENAME}.vst3}"
SYSTEM_VST3_PATH="${SPATIAL_ECHO_SYSTEM_VST3_PATH:-/Library/Audio/Plug-Ins/VST3/${SPATIAL_ECHO_BUNDLE_BASENAME}.vst3}"
USER_AU_PATH="${SPATIAL_ECHO_USER_AU_PATH:-${HOME}/Library/Audio/Plug-Ins/Components/${SPATIAL_ECHO_BUNDLE_BASENAME}.component}"
SYSTEM_AU_PATH="${SPATIAL_ECHO_SYSTEM_AU_PATH:-/Library/Audio/Plug-Ins/Components/${SPATIAL_ECHO_BUNDLE_BASENAME}.component}"

_plist_raw_value() {
    local info="$1"
    local key="$2"

    /usr/bin/plutil -extract "$key" raw -o - "$info" 2>/dev/null
}

# Prints the bundle's executable path derived from Info.plist
# CFBundleExecutable. Fails when the plist or key is missing so callers can
# never hash a hardcoded (possibly wrong-identity) binary path.
bundle_executable_path() {
    local bundle="$1"
    local info="${bundle}/Contents/Info.plist"
    local executable

    if [ ! -f "$info" ]; then
        echo "Error: missing Info.plist: ${info}" >&2
        return 1
    fi

    if ! executable="$(_plist_raw_value "$info" CFBundleExecutable)" \
        || [ -z "$executable" ]; then
        echo "Error: Info.plist has no CFBundleExecutable: ${info}" >&2
        return 1
    fi

    printf '%s/Contents/MacOS/%s\n' "$bundle" "$executable"
}

# Prints "identifier=<id> executable=<name> version=<version>" for a bundle,
# with "(missing)" for absent keys. Fails only when Info.plist is absent.
bundle_identity() {
    local bundle="$1"
    local info="${bundle}/Contents/Info.plist"
    local identifier executable version

    if [ ! -f "$info" ]; then
        echo "Error: missing Info.plist: ${info}" >&2
        return 1
    fi

    identifier="$(_plist_raw_value "$info" CFBundleIdentifier)" || identifier=""
    executable="$(_plist_raw_value "$info" CFBundleExecutable)" || executable=""
    version="$(_plist_raw_value "$info" CFBundleShortVersionString)" || version=""

    printf 'identifier=%s executable=%s version=%s\n' \
        "${identifier:-(missing)}" "${executable:-(missing)}" "${version:-(missing)}"
}

# Prints the architectures of the bundle's derived executable (via lipo),
# or "unavailable" when the executable is missing or not a Mach-O binary.
bundle_architectures() {
    local bundle="$1"
    local binary archs

    if binary="$(bundle_executable_path "$bundle" 2>/dev/null)" \
        && archs="$(lipo -archs "$binary" 2>/dev/null)" \
        && [ -n "$archs" ]; then
        printf '%s\n' "$archs"
    else
        printf 'unavailable\n'
    fi
}

# Prints the AU component triplet "type:subtype:manufacturer" from the
# bundle's Info.plist AudioComponents entry, or "none" when absent.
bundle_au_triplet() {
    local bundle="$1"
    local info="${bundle}/Contents/Info.plist"
    local ctype subtype manufacturer

    ctype="$(_plist_raw_value "$info" 'AudioComponents.0.type')" || ctype=""
    subtype="$(_plist_raw_value "$info" 'AudioComponents.0.subtype')" || subtype=""
    manufacturer="$(_plist_raw_value "$info" 'AudioComponents.0.manufacturer')" || manufacturer=""

    if [ -z "$ctype" ] && [ -z "$subtype" ] && [ -z "$manufacturer" ]; then
        printf 'none\n'
    else
        printf '%s:%s:%s\n' "${ctype:-?}" "${subtype:-?}" "${manufacturer:-?}"
    fi
}

# Prints the SHA-256 of the bundle's derived executable.
hash_bundle_executable() {
    local bundle="$1"
    local binary

    binary="$(bundle_executable_path "$bundle")" || return 1

    if [ ! -f "$binary" ]; then
        echo "Error: bundle executable is missing: ${binary}" >&2
        return 1
    fi

    shasum -a 256 "$binary" | awk '{print $1}'
}
