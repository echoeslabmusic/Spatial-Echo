#!/usr/bin/env python3
"""Compare two TapePresetBaselineRenders metric files across platforms.

The headless runner renders every factory preset with three signals in
deterministic mode and writes preset_metrics.json. Different compilers and
CPUs do not produce bit-identical floating point, so this compares levels and
spectra within tolerances instead of samples.

Usage:
  compare_render_metrics.py <reference.json> <candidate.json> [--report out.json]
Exit status: 0 when every preset/signal metric is within tolerance, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Metric -> (kind, tolerance). "db" compares absolute dB difference, "ratio"
# compares relative difference. Calibrated on arm64 vs x86_64
# (Rosetta) renders of the same source: chaotic presets (random dropouts in
# feedback) move level by up to 0.85 dB, and near-empty bands carry a noise
# floor that differs by architecture. A missing stage, dead channel or level
# jump still fails.
TOLERANCES = {
    "rms_dbfs": ("db", 1.0),
    "rms_r_dbfs": ("db", 1.0),
    "peak_dbfs": ("db", 2.0),
    "band_low_db": ("db", 1.5),
    "band_mid_db": ("db", 1.5),
    "band_high_db": ("db", 1.5),
    "band_air_db": ("db", 1.5),
    "spectral_centroid_hz": ("ratio", 0.15),
    "spectral_centroid_r_hz": ("ratio", 0.15),
}
BANDS = ("band_low_db", "band_mid_db", "band_high_db", "band_air_db")
FLOOR_DB = -80.0
# A band more than this far below the render's loudest band is inaudible
# residue in both files and is not compared.
BAND_RELEVANCE_DB = 40.0


def load(path: Path) -> dict:
    return json.loads(path.read_text())


def rows(data: dict) -> dict[str, dict]:
    return {row["name"]: row for row in data["presets"]}


def compare(reference: dict[str, dict], candidate: dict[str, dict]) -> tuple[list[str], int, dict]:
    failures: list[str] = []
    compared = 0
    worst: dict[str, float] = {}
    for name in sorted(set(reference) | set(candidate)):
        if name not in reference or name not in candidate:
            failures.append(f"{name}: present in only one file")
            continue
        for signal, ref_metrics in reference[name].items():
            if not isinstance(ref_metrics, dict):
                continue
            cand_metrics = candidate[name].get(signal)
            if not isinstance(cand_metrics, dict):
                failures.append(f"{name}/{signal}: missing in candidate")
                continue
            if not cand_metrics.get("finite", False):
                failures.append(f"{name}/{signal}: non-finite candidate output")
                continue
            loudest = max(float(ref_metrics.get(band, FLOOR_DB)) for band in BANDS)
            for metric, (kind, tolerance) in TOLERANCES.items():
                if metric not in ref_metrics or metric not in cand_metrics:
                    continue
                a, b = float(ref_metrics[metric]), float(cand_metrics[metric])
                if metric in BANDS and max(a, b) < loudest - BAND_RELEVANCE_DB:
                    continue
                if kind == "db":
                    if a <= FLOOR_DB and b <= FLOOR_DB:
                        continue
                    difference = abs(a - b)
                else:
                    difference = abs(a - b) / max(abs(a), 1e-9)
                compared += 1
                worst[metric] = max(worst.get(metric, 0.0), difference)
                if difference > tolerance:
                    failures.append(
                        f"{name}/{signal}: {metric} reference {a} candidate {b} "
                        f"(difference {difference:.4g} > {tolerance})"
                    )
    return failures, compared, worst


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--exact", action="store_true",
                        help="require identical metrics (repeatability on one machine)")
    args = parser.parse_args(argv)
    if args.exact:
        for metric in TOLERANCES:
            TOLERANCES[metric] = (TOLERANCES[metric][0], 0.0)

    reference, candidate = load(args.reference), load(args.candidate)
    failures, compared, worst = compare(rows(reference), rows(candidate))
    ref_tree = reference.get("plugin_source_tree")
    cand_tree = candidate.get("plugin_source_tree")
    if ref_tree != cand_tree or str(ref_tree).endswith("-dirty"):
        failures.insert(0, f"reference rendered from plugin/Source {ref_tree}, candidate from {cand_tree}: "
                           "regenerate the reference with scripts/render_metrics.sh on the Mac")
    print(f"reference {reference.get('platform')} vs candidate {candidate.get('platform')}")
    print(f"compared {compared} metrics")
    for metric, value in sorted(worst.items()):
        print(f"  worst {metric}: {value:.4g} (tolerance {TOLERANCES[metric][1]})")
    for failure in failures:
        print(f"FAIL {failure}")
    if args.report:
        args.report.write_text(json.dumps(
            {"compared": compared, "worst": worst, "failures": failures, "pass": not failures},
            indent=2) + "\n")
    print("RENDER MATCH PASSED" if not failures else f"RENDER MATCH FAILED ({len(failures)})")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
