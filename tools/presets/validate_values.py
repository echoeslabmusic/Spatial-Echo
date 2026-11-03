#!/usr/bin/env python3
"""Validate the factory-preset roster.

This is a schema and load-time behavior check. It proves that the roster can
be materialized without clamping, inert story metadata, or reliance on later
automation; it does not prove that the presets sound good.
"""

from __future__ import annotations

import json
import math
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ROSTER = HERE / "roster-values.json"
REFERENCE = HERE / "param-reference.json"

from parameter_manifest import GenerationError, parse_parameter_manifest

ROSTER_SCHEMA = "spatial-echo.roster-values.v2"
REFERENCE_SCHEMA = "spatial-echo.voicing-param-reference.v2"
PARAMETER_COUNT = 64
INTENTIONAL_FACTORY_OMISSIONS = {
    "AutoPanic",
    "TapeNoiseEnabled",
    "SpatialLongDivision",
    "SpatialLongVariantDivision",
}

PREAMP_PROFILES = (
    ("Clean", 0.50, 0.50, 0.50),
    ("Warm", 0.55, 0.65, 0.40),
    ("Bright", 0.55, 0.40, 0.65),
    ("Drive", 0.70, 0.50, 0.50),
    ("Lo-Fi", 0.75, 0.70, 0.25),
)

MIX_FLOORS = {
    "spatial_anchors": 0.52,
    "mix_utilities": 0.45,
    "throws": 0.52,
    "orbits": 0.54,
    "cloud_beds": 0.54,
    "dub_feedback": 0.50,
    "glitch_patterns": 0.50,
    "transitions_impacts": 0.54,
    "psycho_lab": 0.52,
    "motion_lab": 0.52,
}
ROLES = {"hero", "practical", "performance", "specialist"}
AXES = {
    "feedback_decay",
    "head_pattern",
    "stereo_mode",
    "tape_tone",
    "reverb",
    "spatial_route",
    "motion",
    "choreography",
    "spread",
}
PRESET_FIELDS = {
    "number",
    "name",
    "category",
    "role",
    "axes",
    "values",
    "explicit",
    "notes",
}

ECHO_MODES = set(range(0, 11))
REVERB_MODES = set(range(4, 12))
SERIAL_REVERB_MODES = set(range(4, 11))
MOTION_STILL = 0
POSITION_LINKED = 0
CHOREO_UNISON = 0
NOTE_RESTART = 2

MOTION_NAME = re.compile(r"\b(?:orbit|wander|motion|carousel|spiral|helix)\b", re.I)
AUTOMATION_ONLY = re.compile(
    r"\b(?:automate|automation|automated|sweep|sweeps|sweeping|ramp|ramps|ramping)\b",
    re.I,
)
DEFERRED_RISE = re.compile(
    r"\b(?:rise|build)\b.{0,48}\b(?:after|when|once)\b.{0,48}"
    r"\b(?:move|movement|turn|raise|push|macro)\w*\b",
    re.I,
)


def _load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _active_reverb(values: dict[str, Any]) -> bool:
    return int(values["DelaySetting"]) in REVERB_MODES


def _active_echo(values: dict[str, Any]) -> bool:
    return int(values["DelaySetting"]) in ECHO_MODES and values["EchoSend"] == 1


def _active_motion(values: dict[str, Any]) -> bool:
    return (
        values["SpatialEnabled"] == 1
        and values["SpatialMix"] > 0.0
        and int(values["SpatialPattern"]) != MOTION_STILL
        and values["SpatialDepth"] > 0.0
    )


def _axis_is_present(axis: str, values: dict[str, Any]) -> bool:
    """Return whether an axis has an audible/effective stored state at load."""
    if axis == "feedback_decay":
        return (_active_echo(values) and values["Intensity"] > 0.0) or (
            _active_reverb(values) and values["ReverbDecay"] > 0.0
        )
    if axis == "head_pattern":
        return _active_echo(values)
    if axis == "stereo_mode":
        return (
            (_active_echo(values) and values["PingPong"] == 1)
            or (
                values["SpatialEnabled"] == 1
                and values["SpatialMix"] > 0.0
                and values["SpatialSourceWidth"] > 0.0
            )
            or (_active_reverb(values) and values["ReverbWidth"] > 0.0)
        )
    if axis == "tape_tone":
        return _active_echo(values)
    if axis == "reverb":
        return _active_reverb(values)
    if axis == "spatial_route":
        return values["SpatialEnabled"] == 1 and values["SpatialMix"] > 0.0
    if axis == "motion":
        return _active_motion(values)
    if axis in {"choreography", "spread"}:
        return (
            _active_motion(values)
            and int(values["HeadPositionMode"]) != POSITION_LINKED
            and int(values["SpatialChoreography"]) != CHOREO_UNISON
            and values["SpatialSpread"] > 0.0
        )
    raise AssertionError(f"unhandled axis {axis!r}")


def _check_parameter_value(
    preset_label: str,
    parameter_id: str,
    value: Any,
    metadata: dict[str, Any],
    issues: list[str],
) -> bool:
    prefix = f"{preset_label}: {parameter_id}"
    parameter_type = metadata["type"]

    if parameter_type == "bool":
        if value not in (0, 0.0, 1, 1.0, False, True):
            issues.append(f"{prefix}={value!r} is not 0/1")
            return False
        return True

    if not _is_number(value) or not math.isfinite(float(value)):
        issues.append(f"{prefix}={value!r} is not a finite number")
        return False

    if parameter_type == "choice":
        choices = metadata["choices"]
        if value != int(value) or not 0 <= int(value) < len(choices):
            issues.append(
                f"{prefix}={value!r} is not a valid choice index "
                f"(0..{len(choices) - 1})"
            )
            return False
        return True

    value_range = metadata.get("range")
    if value_range is not None:
        low, high = value_range["min"], value_range["max"]
        if not low - 1.0e-9 <= float(value) <= high + 1.0e-9:
            issues.append(f"{prefix}={value!r} is outside [{low}, {high}]")
            return False
        interval = value_range.get("interval")
        if interval is not None:
            if not _is_number(interval) or float(interval) <= 0.0:
                issues.append(f"{prefix} has invalid reference interval {interval!r}")
                return False
            step = (float(value) - float(low)) / float(interval)
            if not math.isclose(step, round(step), rel_tol=0.0, abs_tol=1.0e-6):
                issues.append(
                    f"{prefix}={value!r} is off the {interval:g} interval from {low}"
                )
                return False
    return True


def _validate_reference_against_runtime(
    reference: dict[str, Any], issues: list[str]
) -> None:
    """Prove the hand-curated factory subset matches the live APVTS schema."""
    try:
        manifest = parse_parameter_manifest(ROOT)
    except (OSError, ValueError, GenerationError) as exc:
        issues.append(f"cannot parse live runtime parameter manifest: {exc}")
        return

    live_items = [
        item for item in manifest.get("items", []) if item.get("build_scope") == "release"
    ]
    live_by_id = {item["id"]: item for item in live_items}
    if len(live_by_id) != len(live_items):
        issues.append("live release parameter manifest contains duplicate IDs")
        return

    params = reference["params"]
    reference_ids = set(params)
    live_ids = set(live_by_id)
    unknown_ids = sorted(reference_ids - live_ids)
    if unknown_ids:
        issues.append(f"param-reference IDs absent from live release parameters: {unknown_ids}")

    missing_ids = sorted(live_ids - reference_ids - INTENTIONAL_FACTORY_OMISSIONS)
    if missing_ids:
        issues.append(
            "live release parameters missing from the factory subset without an explicit "
            f"omission: {missing_ids}"
        )

    stale_omissions = sorted(INTENTIONAL_FACTORY_OMISSIONS - live_ids)
    if stale_omissions:
        issues.append(f"intentional factory omissions are absent from runtime: {stale_omissions}")

    for parameter_id in reference["order"]:
        live = live_by_id.get(parameter_id)
        if live is None:
            continue
        metadata = params[parameter_id]
        for field in ("type", "default", "range", "choices", "unit"):
            if metadata.get(field) != live.get(field):
                issues.append(
                    f"{parameter_id}: reference {field}={metadata.get(field)!r} "
                    f"differs from runtime {live.get(field)!r}"
                )


def _check_preset(
    preset: Any,
    order: list[str],
    params: dict[str, Any],
    issues: list[str],
) -> None:
    if not isinstance(preset, dict):
        issues.append(f"preset entry {preset!r} is not an object")
        return

    number = preset.get("number", "?")
    name = preset.get("name", "<unnamed>")
    label = f"#{number} {name}"

    fields = set(preset)
    if fields != PRESET_FIELDS:
        missing = sorted(PRESET_FIELDS - fields)
        extra = sorted(fields - PRESET_FIELDS)
        issues.append(f"{label}: preset fields differ (missing={missing}, extra={extra})")
        if "values" not in preset:
            return

    if not isinstance(name, str) or not name.strip():
        issues.append(f"{label}: name must be a non-empty string")
    elif len(name) > 20:
        issues.append(f"{label}: name is {len(name)} characters; maximum is 20")

    category = preset.get("category")
    if not isinstance(category, str) or category not in MIX_FLOORS:
        issues.append(f"{label}: unknown category {category!r}")

    role = preset.get("role")
    if not isinstance(role, str) or role not in ROLES:
        issues.append(f"{label}: role {role!r} is not one of {sorted(ROLES)}")

    axes = preset.get("axes")
    valid_axes: list[str] = []
    if not isinstance(axes, list):
        issues.append(f"{label}: axes must be an array")
    else:
        if len(axes) < 3:
            issues.append(f"{label}: axes has {len(axes)} entries; minimum is 3")
        non_string_axes = [axis for axis in axes if not isinstance(axis, str)]
        if non_string_axes:
            issues.append(f"{label}: axes must contain only strings: {non_string_axes!r}")
        string_axes = [axis for axis in axes if isinstance(axis, str)]
        if len(string_axes) != len(set(string_axes)):
            issues.append(f"{label}: axes contains duplicates")
        unknown_axes = sorted({axis for axis in string_axes if axis not in AXES})
        if unknown_axes:
            issues.append(f"{label}: unknown axes {unknown_axes}")
        valid_axes = [axis for axis in string_axes if axis in AXES]

    notes = preset.get("notes")
    if not isinstance(notes, str) or not notes.strip():
        issues.append(f"{label}: notes must describe the stored load-time behavior")
    elif AUTOMATION_ONLY.search(notes) or DEFERRED_RISE.search(notes):
        issues.append(
            f"{label}: notes describe automation or deferred macro movement; "
            "factory identity must be present at load"
        )

    explicit = preset.get("explicit")
    if not isinstance(explicit, list):
        issues.append(f"{label}: explicit must be an ordered parameter array")
    elif explicit != order:
        issues.append(f"{label}: explicit parameter order does not match param-reference.json")

    values = preset.get("values")
    if not isinstance(values, dict):
        issues.append(f"{label}: values must be an object")
        return
    if list(values) != order:
        missing = sorted(set(order) - set(values))
        extra = sorted(set(values) - set(order))
        issues.append(
            f"{label}: values parameter order does not match param-reference.json "
            f"(missing={missing}, extra={extra})"
        )
        return

    value_checks = [
        _check_parameter_value(
            label, parameter_id, values[parameter_id], params[parameter_id], issues
        )
        for parameter_id in order
    ]

    # The checks below require already-legal numeric values. Avoid secondary
    # exceptions when reporting a malformed row.
    if not all(value_checks):
        return

    preamp_index = int(values["PreampPreset"])
    if 0 <= preamp_index < len(PREAMP_PROFILES):
        profile_name, expected_input, expected_bass, expected_treble = PREAMP_PROFILES[
            preamp_index
        ]
        actual = (values["InputLevel"], values["Bass"], values["Treble"])
        expected = (expected_input, expected_bass, expected_treble)
        if any(
            not math.isclose(float(got), wanted, rel_tol=0.0, abs_tol=1.0e-6)
            for got, wanted in zip(actual, expected)
        ):
            issues.append(
                f"{label}: PreampPreset {profile_name} requires "
                f"Input/Bass/Treble={expected}, got {actual}; use Custom for bespoke EQ"
            )

    for switch in ("Power", "EchoSend", "OutputProtection", "FeedbackCompensation"):
        if values[switch] != 1:
            issues.append(f"{label}: {switch} must be on at load")
    if not math.isclose(float(values["OutputGain"]), 0.5, abs_tol=1.0e-9):
        issues.append(f"{label}: OutputGain must be 0.5, got {values['OutputGain']!r}")
    if values["Intensity"] >= 0.90:
        issues.append(f"{label}: Intensity {values['Intensity']} must be below 0.90")
    if isinstance(category, str) and category in MIX_FLOORS and values["Mix"] < MIX_FLOORS[category] - 1.0e-9:
        issues.append(
            f"{label}: Mix {values['Mix']} is below {category} floor {MIX_FLOORS[category]}"
        )

    pattern = int(values["SpatialPattern"])
    motion_declared = bool(MOTION_NAME.search(str(name))) or bool(
        {"motion", "choreography", "spread"}.intersection(valid_axes)
    )
    if pattern != MOTION_STILL or motion_declared:
        if values["SpatialEnabled"] != 1:
            issues.append(f"{label}: motion is named/declared but SpatialEnabled is off")
        if pattern == MOTION_STILL:
            issues.append(f"{label}: motion is named/declared but SpatialPattern is Still")
        if values["SpatialDepth"] <= 0.0:
            issues.append(f"{label}: motion is named/declared but SpatialDepth is zero")
    if values["SpatialSync"] == 1 and pattern == MOTION_STILL:
        issues.append(f"{label}: SpatialSync is on while SpatialPattern is Still")
    if int(values["SpatialRestart"]) == NOTE_RESTART:
        issues.append(
            f"{label}: SpatialRestart Note needs MIDI, which only the Standalone receives"
        )

    position_mode = int(values["HeadPositionMode"])
    choreography = int(values["SpatialChoreography"])
    spread = values["SpatialSpread"]
    if position_mode != POSITION_LINKED and (
        values["SpatialEnabled"] != 1 or values["SpatialMix"] <= 0.0
    ):
        issues.append(
            f"{label}: Independent head positions require audible spatial processing at load"
        )
    if choreography != CHOREO_UNISON or spread > 0.0:
        if position_mode == POSITION_LINKED:
            issues.append(f"{label}: choreography/spread is inert in Linked head-position mode")
        if not _active_motion(values):
            issues.append(f"{label}: choreography/spread requires audible motion at load")
        if choreography == CHOREO_UNISON:
            issues.append(f"{label}: SpatialSpread is nonzero but choreography is Unison")
        if spread <= 0.0:
            issues.append(f"{label}: non-Unison choreography has zero SpatialSpread")

    if int(values["ReverbRouting"]) != 0 and int(values["DelaySetting"]) not in SERIAL_REVERB_MODES:
        issues.append(f"{label}: serial reverb routing requires an echo+reverb DelaySetting")

    for axis in valid_axes:
        if not _axis_is_present(axis, values):
            issues.append(f"{label}: declared axis {axis!r} is not effective at load")


def validate_table(table: Any, reference: Any) -> list[str]:
    """Return all schema, range, and load-time behavior issues."""
    issues: list[str] = []

    if not isinstance(reference, dict):
        return ["param-reference.json root is not an object"]
    if reference.get("schema") != REFERENCE_SCHEMA:
        issues.append(
            f"param-reference schema is {reference.get('schema')!r}, expected {REFERENCE_SCHEMA!r}"
        )
    order = reference.get("order")
    params = reference.get("params")
    if not isinstance(order, list) or not isinstance(params, dict):
        issues.append("param-reference.json must contain order array and params object")
        return issues
    if len(order) != PARAMETER_COUNT or len(params) != PARAMETER_COUNT:
        issues.append(
            f"param-reference has {len(order)} ordered IDs/{len(params)} definitions; "
            f"expected {PARAMETER_COUNT}"
        )
    if list(params) != order:
        issues.append("param-reference params object order does not match order array")
    if len(order) != len(set(order)):
        issues.append("param-reference order contains duplicate parameter IDs")
    missing_metadata = sorted(set(order) - set(params))
    extra_metadata = sorted(set(params) - set(order))
    if missing_metadata or extra_metadata:
        issues.append(
            f"param-reference order/params differ (missing={missing_metadata}, extra={extra_metadata})"
        )
        return issues

    _validate_reference_against_runtime(reference, issues)

    if not isinstance(table, dict):
        return issues + ["roster-values.json root is not an object"]
    expected_table_fields = {"schema", "parameter_order", "presets"}
    if set(table) != expected_table_fields:
        issues.append(
            "roster top-level fields differ "
            f"(missing={sorted(expected_table_fields - set(table))}, "
            f"extra={sorted(set(table) - expected_table_fields)})"
        )
    if table.get("schema") != ROSTER_SCHEMA:
        issues.append(f"roster schema is {table.get('schema')!r}, expected {ROSTER_SCHEMA!r}")
    if table.get("parameter_order") != order:
        issues.append("roster parameter_order does not match param-reference.json")

    presets = table.get("presets")
    if not isinstance(presets, list):
        return issues + ["roster presets must be an array"]
    if not presets:
        return issues + ["roster presets must not be empty"]

    numbers = [preset.get("number") if isinstance(preset, dict) else None for preset in presets]
    if numbers != list(range(1, len(presets) + 1)):
        issues.append(f"preset numbers are not contiguous 1..{len(presets)}: {numbers}")

    for preset in presets:
        _check_preset(preset, order, params, issues)

    names = [preset.get("name") for preset in presets if isinstance(preset, dict)]
    normalized_names = [name.casefold() for name in names if isinstance(name, str)]
    duplicate_names = sorted(
        {name for name in normalized_names if normalized_names.count(name) > 1}
    )
    if duplicate_names:
        issues.append(f"duplicate preset names (case-insensitive): {duplicate_names}")

    return issues


def main() -> int:
    try:
        reference = _load_json(REFERENCE)
        table = _load_json(ROSTER)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"validation setup failed: {exc}")
        return 1

    issues = validate_table(table, reference)
    presets = table.get("presets", []) if isinstance(table, dict) else []
    categories = Counter(
        preset.get("category")
        if isinstance(preset.get("category"), str)
        else repr(preset.get("category"))
        for preset in presets
        if isinstance(preset, dict)
    )
    print(f"presets: {len(presets)}  categories: {dict(categories)}")
    if issues:
        print(f"\n{len(issues)} ERROR(S):")
        for issue in issues:
            print(" -", issue)
        return 1

    print("clean pass (schema, values, and load-time behavior)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
