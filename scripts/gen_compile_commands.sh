#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-macos-clangd}"
BUILD_DIR="${ROOT_DIR}/build/cmake/${PRESET}"
COMPILE_DB="${BUILD_DIR}/compile_commands.json"

cmake --fresh --preset "$PRESET"

if [ ! -f "$COMPILE_DB" ]; then
  echo "Compilation database not generated at ${COMPILE_DB}." >&2
  exit 1
fi

ln -sf "$COMPILE_DB" "${ROOT_DIR}/compile_commands.json"
echo "compile_commands.json -> ${COMPILE_DB}"
