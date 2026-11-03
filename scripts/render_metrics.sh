#!/bin/bash
# Renders every factory preset through the Release headless runner in
# deterministic mode and writes the per-preset level/spectrum metrics, stamped
# with the plugin/Source tree they came from. The same script runs
# on macOS and in Git Bash on Windows.
# Usage: ./scripts/render_metrics.sh <headless-runner> <out.json> [wav-dir]
# With wav-dir, the WAV renders are kept there for diagnosis.

set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 <headless-runner> <out.json> [wav-dir]" >&2
    exit 2
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
RUNNER="$1"
OUT="$2"
WAV_DIR="${3:-}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/spatial-echo-render.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

SPATIAL_ECHO_DETERMINISTIC=1 \
SPATIAL_ECHO_HEADLESS_TEST=1 \
SPATIAL_ECHO_RENDER_METRICS_ONLY="$([ -z "$WAV_DIR" ] && echo 1)" \
SPATIAL_ECHO_TEST_ARTIFACT_DIR="$WORK" \
    "$RUNNER" --test TapePresetBaselineRenders

if [ -n "$WAV_DIR" ]; then
    mkdir -p "$WAV_DIR"
    cp "$WORK"/tape-baseline/preset-renders/*.wav "$WAV_DIR"/
fi

TREE="$(git -C "$ROOT_DIR" rev-parse HEAD:plugin/Source)"
DIRTY="$(git -C "$ROOT_DIR" status --porcelain -- plugin/Source | wc -l | tr -d ' ')"
python3 - "$WORK/tape-baseline/preset_metrics.json" "$OUT" "$TREE" "$DIRTY" <<'PY'
import json, platform, sys
src, out, tree, dirty = sys.argv[1:5]
data = json.load(open(src))
data["plugin_source_tree"] = tree if dirty == "0" else tree + "-dirty"
data["platform"] = f"{platform.system()}-{platform.machine()}"
json.dump(data, open(out, "w"), indent=1)
PY
echo "metrics: $OUT (plugin/Source tree $TREE, uncommitted files: $DIRTY)"
