#!/usr/bin/env bash
# Apply (or with --check, verify) clang-format on first-party C++ sources.
# Vendored code under plugin/Source/DSP/dependencies/<library>/ and the
# generated FactoryPresetLibrary.h are left alone.
set -euo pipefail

cd "$(dirname "$0")/.."

mode="apply"
if [ "${1:-}" = "--check" ]; then
    mode="check"
fi

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

files="$(git ls-files -- 'plugin/Source/*.h' 'plugin/Source/*.cpp' 'tools/*.h' 'tools/*.cpp' \
    | grep -vE '^plugin/Source/DSP/dependencies/[^/]+/' \
    | grep -v '^plugin/Source/Presets/FactoryPresetLibrary.h$')"

if [ -z "$files" ]; then
    echo "no sources found" >&2
    exit 1
fi

if [ "$mode" = "check" ]; then
    echo "$files" | xargs "$CLANG_FORMAT" --dry-run --Werror
    echo "formatting OK"
else
    echo "$files" | xargs "$CLANG_FORMAT" -i
    echo "formatted $(echo "$files" | wc -l | tr -d ' ') files"
fi
