#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# plugin/Source/DSP/dependencies/ holds first-party hot paths (Biquad,
# CircularBuffer, the spring and tape models) beside the vendored libraries,
# so the whole folder is scanned.
exec python3 "$ROOT_DIR/tools/python_audit/rt_safety_audit.py" --root "$ROOT_DIR" --include-dependencies "$@"
