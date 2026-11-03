#!/usr/bin/env python3
"""Check that preset names and stories match their stored values.

Every preset is checked for literal name claims such as ``Orbit`` or
``Whole-Note`` being true immediately after load. Presets with a concrete
recipe story (fixed answers, stationary sources, spatial showcases) are also
checked against that recipe, matched by name, so presets can be edited,
reordered or appended freely.
"""

from __future__ import annotations

import json
import re
import sys
from typing import Any

from validate_values import REFERENCE, ROSTER, validate_table

# DelaySetting index -> audible delay heads. Mode 11 is Reverb Only.
ACTIVE_HEADS = {
    0: (1,),
    1: (2,),
    2: (3,),
    3: (2, 3),
    4: (1,),
    5: (2,),
    6: (3,),
    7: (1, 2),
    8: (2, 3),
    9: (1, 3),
    10: (1, 2, 3),
    11: (),
}

# Fixed-answer recipes: stored H1/H2/H3 note divisions.
ANSWER_DIVISIONS = {
    "Cloud Packets": (1, 12, 0),
    "Forest Afterglow": (2, 13, 1),
    "Crosscurrent": (9, 3, 8),
    "Offbeat Weave": (15, 3, 13),
    "Velvet Answer": (2, 13, 1),
    "Pond Round": (9, 8, 2),
    "Distant Lantern": (7, 1, 12),
    "Spring Aftershock": (3, 14, 2),
}

# Stationary-source recipes: loaded DelaySetting (head/reverb mode).
STATIONARY_MODES = {
    "Bass Reply": 0, "Hat Ghost Triplets": 3, "Quarter Adlib": 0,
    "Halfstep Shadow": 0, "Riddim Pocket Room": 11, "Lead Gap Cloud": 11,
    "Canon Windows": 3, "Bell Counterphrase": 0, "Concrete Chatter": 0,
    "Hidden Rhythm": 0, "Static Sweep": 3, "Spatial Pull": 10,
}

# Spatial showcase recipes: SpatialPattern index.
SHOWCASE_PATTERNS = {
    "Point Orbit": 1, "Beat Orbit": 1, "Front Swing": 2,
    "Near Ear Pass": 2, "Side Relay": 0, "Orbit Replies": 1,
    "Approach Retreat": 4, "Near Far Relay": 0, "Rear Window": 2,
    "Front Back Relay": 0, "Height Relay": 0, "Sky Figure Eight": 3,
}



def _canonical(text: str) -> str:
    return re.sub(r"[^a-z0-9]", "", text.casefold())


def _active_motion(values: dict[str, Any], pattern: int | None = None) -> bool:
    return (
        values["SpatialEnabled"] == 1
        and values["SpatialDepth"] > 0.0
        and int(values["SpatialPattern"]) != 0
        and (pattern is None or int(values["SpatialPattern"]) == pattern)
    )


def _active_divisions(values: dict[str, Any]) -> list[int]:
    heads = ACTIVE_HEADS[int(values["DelaySetting"])]
    return [int(values[f"Head{head}NoteDivision"]) for head in heads]


def _has_active_elevation(values: dict[str, Any]) -> bool:
    if values["SpatialEnabled"] != 1:
        return False
    if int(values["HeadPositionMode"]) == 0:
        return abs(float(values["SpatialElevation"])) > 1.0e-9
    active_heads = ACTIVE_HEADS[int(values["DelaySetting"])]
    return any(
        abs(float(values[f"Head{head}Elevation"])) > 1.0e-9
        for head in active_heads
    )


def _is_dark(values: dict[str, Any], defaults: dict[str, Any]) -> bool:
    return (
        values["Treble"] < defaults["Treble"]
        or (
            int(values["DelaySetting"]) >= 4
            and values["ReverbTone"] < defaults["ReverbTone"]
        )
    )


def _check_named_claims(
    preset: dict[str, Any], defaults: dict[str, Any], issues: list[str]
) -> None:
    name = preset["name"]
    values = preset["values"]
    axes = set(preset["axes"])
    canonical_name = _canonical(name)
    label = f"{name} ({preset['category']}/{preset['role']})"

    def require(condition: bool, message: str) -> None:
        if not condition:
            issues.append(f"{label}: {message}")

    if "pingpong" in canonical_name:
        require(values["PingPong"] == 1, "PingPong name requires PingPong on")
        require(bool(_active_divisions(values)), "PingPong name requires an audible echo head")
        require("stereo_mode" in axes, "PingPong name requires stereo_mode in axes")

    if "orbit" in canonical_name:
        require(_active_motion(values, pattern=1), "Orbit name requires enabled Orbit motion at nonzero depth")
        require("motion" in axes, "Orbit name requires motion in axes")

    if "wander" in canonical_name:
        require(_active_motion(values, pattern=5), "Wander name requires enabled Wander motion at nonzero depth")
        require("motion" in axes, "Wander name requires motion in axes")

    if "elevation" in canonical_name:
        require(_has_active_elevation(values), "Elevation name requires a nonzero active elevation coordinate")
        require("spatial_route" in axes, "Elevation name requires spatial_route in axes")

    if "dark" in canonical_name:
        require(_is_dark(values, defaults), "Dark name requires stored treble or active reverb-tone darkening")
        require(bool({"tape_tone", "reverb"}.intersection(axes)), "Dark name requires a tone axis")

    if "wholenote" in canonical_name:
        require(values["TempoSync"] == 1, "Whole-Note name requires TempoSync on")
        require(0 in _active_divisions(values), "Whole-Note name requires an active 1/1 delay head")
        require("head_pattern" in axes, "Whole-Note name requires head_pattern in axes")

    # Engine words in either the product name or its concise behavior story
    # are literal, not mood copy. They must identify the loaded reverb engine.
    story = f"{name} {preset['notes']}"
    mentions_spring = bool(re.search(r"\bspring\b", story, re.I))
    mentions_cloud = bool(re.search(r"\bcloud\b", story, re.I))
    if mentions_spring and mentions_cloud:
        issues.append(f"{label}: story claims both Spring and Cloud")
    elif mentions_spring:
        require(int(values["DelaySetting"]) >= 4, "Spring story requires an audible reverb mode")
        require(int(values["ReverbEngine"]) == 0, "Spring story requires ReverbEngine Spring")
        require("reverb" in axes, "Spring story requires reverb in axes")
    elif mentions_cloud:
        require(int(values["DelaySetting"]) >= 4, "Cloud story requires an audible reverb mode")
        require(int(values["ReverbEngine"]) == 1, "Cloud story requires ReverbEngine Cloud")
        require("reverb" in axes, "Cloud story requires reverb in axes")


def _check_answer_story(preset: dict[str, Any], issues: list[str]) -> None:
    """Keep the fixed-answer recipes' route, timing and distance identities true."""
    name, values = preset["name"], preset["values"]

    def require(condition: bool, message: str) -> None:
        if not condition:
            issues.append(f"{name}: {message}")

    require(values["TempoSync"] == 1, "fixed-answer recipes require TempoSync")
    require(values["HeadPositionMode"] == 1, "dry foreground requires Independent heads")
    require(values["SpatialPattern"] == 0, "fixed-answer story requires Still positions")
    require(values["PingPong"] == 0, "fixed head answers must not use feedback PingPong")
    require(values["Intensity"] <= 0.30, "first-answer identity requires conservative feedback")

    divisions = tuple(int(values[f"Head{head}NoteDivision"]) for head in (1, 2, 3))
    require(
        divisions == ANSWER_DIVISIONS[name],
        "stored head divisions differ from the answer story",
    )
    require(
        values["DelaySetting"] == (7 if name == "Velvet Answer" else 10),
        "loaded head combination differs from the answer story",
    )

    if name in {"Cloud Packets", "Spring Aftershock"}:
        require(values["ReverbRouting"] == 2, "returning ambience requires Reverb to Echo")
        require(
            values["ReverbDecay"] <= 0.12,
            "separate returning textures require compact decay",
        )
    elif name in {"Forest Afterglow", "Velvet Answer"}:
        require(values["ReverbRouting"] == 1, "echo-fed ambience requires Echo to Reverb")

    if name == "Forest Afterglow":
        require(
            values["Treble"] < 0.5 and values["ReverbTone"] < 0.5,
            "softened afterglow requires darker echo and reverb tone",
        )
    if name == "Velvet Answer":
        require(values["TapeFlavor"] == 1, "warm answers require WARM tape")
        require(
            values["ReverbDuckAmount"] > 0,
            "ducked reverb story requires active reverb ducking",
        )
    if name == "Pond Round":
        positions = {values[f"Head{head}Azimuth"] for head in (1, 2, 3)}
        require(
            len(positions) == 3,
            "turn-taking answers require three distinct fixed positions",
        )
    if name == "Distant Lantern":
        require(
            all(values[f"Head{head}Distance"] >= 3.0 for head in (1, 2, 3)),
            "distant-answer story requires all echo heads at least 3 m",
        )


def _check_stationary_story(preset: dict[str, Any], issues: list[str]) -> None:
    """Verify the stationary-source recipes' audible modes, timing and fixed positions."""
    name, values = preset["name"], preset["values"]

    def require(condition: bool, message: str) -> None:
        if not condition:
            issues.append(f"{name}: {message}")

    expected_divisions = {
        "Bass Reply": [3], "Hat Ghost Triplets": [11, 10],
        "Quarter Adlib": [2], "Halfstep Shadow": [2],
        "Riddim Pocket Room": [], "Lead Gap Cloud": [],
        "Canon Windows": [1, 0], "Bell Counterphrase": [12],
        "Hidden Rhythm": [2], "Static Sweep": [10, 4], "Spatial Pull": [5, 4, 15],
    }
    require(values["DelaySetting"] == STATIONARY_MODES[name], "loaded head/reverb mode differs from the recipe")
    require(values["SpatialPattern"] == 0 and values["SpatialDepth"] == 0, "stationary-source story requires Still with zero trajectory depth")
    require(values["SpatialSync"] == 0, "stationary-source story must not sync a trajectory")
    require(values["PingPong"] == 0, "discrete fixed replies must not alternate through PingPong feedback")
    require(values["ReverbRouting"] == 0, "stationary-source recipes use parallel or echo-only routing")

    free_or_reverb_only = {"Concrete Chatter", "Riddim Pocket Room", "Lead Gap Cloud"}
    require(values["TempoSync"] == (0 if name in free_or_reverb_only else 1), "tempo-sync state differs from the recipe")
    if name == "Concrete Chatter":
        base_ms = 175.0 - 105.0 * float(values["RepeatRate"]) ** 0.6
        require(80.0 <= base_ms <= 100.0, "architectural chatter requires an 80–100 ms free delay")
    else:
        require(_active_divisions(values) == expected_divisions[name], "active divisions differ from the rhythmic story")

    no_regeneration = {
        "Bass Reply", "Hat Ghost Triplets", "Riddim Pocket Room", "Lead Gap Cloud",
        "Canon Windows", "Hidden Rhythm", "Static Sweep", "Spatial Pull",
    }
    if name in no_regeneration:
        require(values["Intensity"] == 0, "finite first-pass/reverb-only story requires zero regeneration")
    feedback_bounds = {
        "Quarter Adlib": (0.18, 0.25), "Halfstep Shadow": (0.10, 0.18),
        "Bell Counterphrase": (0.05, 0.12), "Concrete Chatter": (0.35, 0.45),
    }
    if name in feedback_bounds:
        low, high = feedback_bounds[name]
        require(low <= values["Intensity"] <= high, "stored regeneration differs from the intended decay role")

    unspatialized = {"Halfstep Shadow", "Riddim Pocket Room", "Lead Gap Cloud", "Concrete Chatter"}
    if name in unspatialized:
        require(values["SpatialEnabled"] == 0 and values["HeadPositionMode"] == 0, "source-layout story requires spatial processing off and Linked mode")
    else:
        require(values["SpatialEnabled"] == 1 and values["HeadPositionMode"] == 1 and values["SpatialMix"] > 0, "fixed separate-source story requires audible Independent head positions")

    clean_tape = {"Bass Reply", "Hat Ghost Triplets", "Canon Windows", "Concrete Chatter", "Hidden Rhythm", "Static Sweep", "Spatial Pull"}
    if name in clean_tape:
        require(values["TapeFlavor"] == 0, "clean-answer story requires CLEAN tape")
    elif name in {"Quarter Adlib", "Halfstep Shadow", "Bell Counterphrase"}:
        require(values["TapeFlavor"] == 1, "warm-answer story requires WARM tape")
    if name in {"Canon Windows", "Hidden Rhythm", "Static Sweep", "Spatial Pull"}:
        require(values["Bass"] == 0.5 and values["Treble"] == 0.5, "flat-shelf story requires neutral echo shelves")

    if name in {"Hat Ghost Triplets", "Canon Windows", "Static Sweep"}:
        require(values["Head2Distance"] == values["Head3Distance"], "two fixed answers require equal stored distances")
        require(values["Head2Azimuth"] != values["Head3Azimuth"], "two fixed answers require distinct positions")
    if name == "Hat Ghost Triplets":
        require(values["EchoLevel"] <= -8.0, "quiet ghost accents require a restrained echo trim")
    if name == "Halfstep Shadow":
        require(values["EchoLevel"] <= -6.0, "quiet percussion answers require a restrained echo trim")
    if name == "Bell Counterphrase":
        require(values["Head1Distance"] >= 1.5 and values["Head1Azimuth"] < 0, "late bell answer requires a more distant left-side position")
        require(values["Bass"] < 0.5 and values["Treble"] < 0.5, "dark bell answer requires cut echo shelves")
    if name == "Concrete Chatter":
        require(values["Bass"] < 0.5 and values["Treble"] < 0.5, "hollow chatter requires cuts to both echo shelves")
    if name == "Riddim Pocket Room":
        require(values["ReverbEngine"] == 1 and values["ReverbDecay"] <= 0.18 and values["ReverbSize"] <= 0.18, "compact room story requires a short small Cloud")
        require(values["ReverbLevel"] <= -10.0 and values["ReverbDuckAmount"] > 0, "quiet attack-support role requires low direct reverb level and ducking")
    if name == "Lead Gap Cloud":
        require(values["ReverbEngine"] == 1 and 0.35 <= values["ReverbDecay"] <= 0.45, "lead-tail role requires a moderate Cloud decay")
        require(values["ReverbWidth"] >= 0.80 and values["ReverbDuckAmount"] >= 0.65, "wide ducked-tail story requires width and dry-keyed ducking")
    if name == "Spatial Pull":
        require(values["Head1Azimuth"] == values["Head2Azimuth"] < values["Head3Azimuth"], "spatial comparison requires the first location repeated before switching")
        require(values["Head1Distance"] == values["Head2Distance"] == values["Head3Distance"], "spatial sequence requires equal stored head distances")
        require(values["ReverbEngine"] == 1 and values["ReverbDecay"] == 0 and values["ReverbLevel"] == -12.0, "required all-head ambience must use minimum-decay Cloud at minus 12 dB")


def _check_showcase_story(preset: dict[str, Any], issues: list[str]) -> None:
    """Keep focused trajectories and finite directional contrasts audible."""
    name, values = preset["name"], preset["values"]

    def require(condition: bool, message: str) -> None:
        if not condition:
            issues.append(f"{name}: {message}")

    pairs = {"Side Relay", "Orbit Replies", "Near Far Relay", "Front Back Relay", "Height Relay"}
    require(values["SpatialEnabled"] == 1 and values["SpatialMix"] == 100,
            "showcase requires fully audible binaural processing")
    require(values["SpatialSourceWidth"] == 0, "point-source story requires zero width blend")
    require(values["SpatialQuality"] == 2 and values["SpatialCrossover"] == 100,
            "showcase requires the declared high-quality binaural band split")
    require(values["Power"] == values["EchoSend"] == 1 and values["EchoLevel"] == -6,
            "showcase copy must remain enabled at its declared level")
    require(values["Intensity"] == 0 and values["PingPong"] == 0,
            "finite focused copies require zero regeneration and no PingPong")
    require(values["TapeFlavor"] == 0 and values["Bass"] == values["Treble"] == .5,
            "cue-preserving story requires clean tape and flat echo shelves")
    require(values["DelaySetting"] == (3 if name in pairs else 0),
            "showcase head selection must exclude added reverb")
    require(values["Mix"] == (.9 if name == "Side Relay" else 1),
            "focused return differs from the declared dry/wet balance")
    require(values["SpatialRouting"] == 0 and values["ReverbRouting"] == 0,
            "showcase requires After Effects spatial routing and no serial reverb")
    require(values["HeadPositionMode"] == (1 if name in pairs else 0),
            "moving-source versus separate-head routing differs from the story")
    require(values["TempoSync"] == (1 if name in pairs else 0),
            "short free delay versus rhythmic reply clock differs from the story")
    if name in pairs:
        require(_active_divisions(values) == [3, 13],
                "separated replies require eighth and dotted-quarter divisions")
    else:
        require(values["RepeatRate"] == 1, "single-object copy requires the 70 ms free delay")

    patterns = SHOWCASE_PATTERNS
    require(values["SpatialPattern"] == patterns[name], "trajectory differs from its named behavior")
    require(values["SpatialSync"] == (1 if name == "Beat Orbit" else 0),
            "free versus host-synced motion differs from the story")
    require(values["SpatialRestart"] == (1 if name == "Beat Orbit" else 0),
            "showcase motion must not rely on MIDI or random restart")
    expected_base = 180 if name in {"Beat Orbit", "Rear Window"} else (
        75 if name == "Approach Retreat" else 0)
    if name not in pairs:
        require(values["SpatialAzimuth"] == expected_base and values["SpatialElevation"] == 0,
                "named moving path must retain its base bearing and zero elevation")
        expected_distance = .35 if name == "Near Ear Pass" else (
            1.9 if name == "Approach Retreat" else 1.1)
        require(values["SpatialDistance"] == expected_distance,
                "named moving path must retain its declared base radius")
    rates = {"Point Orbit": .16, "Front Swing": .2, "Near Ear Pass": .24,
             "Orbit Replies": .1, "Approach Retreat": .16,
             "Rear Window": .16, "Sky Figure Eight": .12}
    if name in rates:
        require(values["SpatialRate"] == rates[name], "named cycle duration differs from the story")
    if patterns[name] == 2:
        require(values["SpatialDepth"] == 100, "60-degree arc requires full Pendulum depth")
    if patterns[name] == 0:
        require(values["SpatialDepth"] == 0, "fixed comparisons must remain still")
    if name == "Beat Orbit":
        require(values["SpatialDivision"] == 0, "beat orbit requires one cycle per whole note")
    if patterns[name] == 1:
        require(values["SpatialDepth"] == 1, "horizontal orbit requires small positive depth")
    if name == "Orbit Replies":
        require(values["SpatialChoreography"] == 2 and values["SpatialSpread"] == 100,
                "separate orbit phases require full Cascade spread")
        require(values["Head2Azimuth"] == values["Head3Azimuth"] == 0,
                "Cascade phase comparison requires common base bearings")
    else:
        require(values["SpatialChoreography"] == 0 and values["SpatialSpread"] == 0,
                "single or fixed positions must not use inert choreography")
    if name == "Approach Retreat":
        require(values["SpatialDistance"] == 1.9 and values["SpatialDepth"] == 100,
                "0.4–3.4 m cue cycle requires the unclamped Breathe range")
        require(values["SpatialAzimuth"] == 75, "lateral distance cycle must retain its angle")
    if name == "Near Far Relay":
        require(values["Head2Distance"] == .4 and values["Head3Distance"] == 4,
                "near/far contrast requires the two declared distance controls")
        require(values["Head2Azimuth"] == values["Head3Azimuth"] == 75
                and values["Head2Elevation"] == values["Head3Elevation"] == 0,
                "depth contrast must hold azimuth and elevation fixed")
    if name == "Side Relay":
        require(values["Head2Azimuth"] == -75 and values["Head3Azimuth"] == 75,
                "side replies require opposite lateral positions")
    if name in {"Side Relay", "Orbit Replies", "Front Back Relay"}:
        require(values["Head2Elevation"] == values["Head3Elevation"] == 0
                and values["Head2Distance"] == values["Head3Distance"] == 1.1,
                "lateral/front-rear comparisons require equal height and radius")
    if name == "Front Back Relay":
        require(values["Head2Azimuth"] == 0 and values["Head3Azimuth"] == 180,
                "front/rear experiment requires distinct median-plane directions")
    if name == "Height Relay":
        require(values["Head2Elevation"] == -35 and values["Head3Elevation"] == 55,
                "height comparison requires low/high direction settings")
        require(values["Head2Azimuth"] == values["Head3Azimuth"] == 45
                and values["Head2Distance"] == values["Head3Distance"] == 1.1,
                "height comparison must hold azimuth and distance fixed")
    if name == "Rear Window":
        require(values["SpatialAzimuth"] == 180, "rear arc requires its rear-centered base")
    if name == "Near Ear Pass":
        require(values["SpatialDistance"] == .35, "close pass requires the declared near-field setting")
    if name == "Sky Figure Eight":
        require(values["SpatialDepth"] == 85, "51-degree figure-eight requires the declared depth")


def check_story(table: dict[str, Any], reference: dict[str, Any]) -> list[str]:
    """Return literal name-claim and recipe-story mismatches."""
    issues: list[str] = []
    schema_issues = validate_table(table, reference)
    if schema_issues:
        return ["roster validation must pass before story checks"] + schema_issues

    defaults = {
        parameter_id: metadata["default"]
        for parameter_id, metadata in reference["params"].items()
    }
    for preset in table["presets"]:
        _check_named_claims(preset, defaults, issues)
        name = preset["name"]
        if name in ANSWER_DIVISIONS:
            _check_answer_story(preset, issues)
        elif name in STATIONARY_MODES:
            _check_stationary_story(preset, issues)
        elif name in SHOWCASE_PATTERNS:
            _check_showcase_story(preset, issues)

    return issues


def main() -> int:
    try:
        reference = json.loads(REFERENCE.read_text(encoding="utf-8"))
        table = json.loads(ROSTER.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"story-check setup failed: {exc}")
        return 1

    issues = check_story(table, reference)
    if issues:
        print(f"{len(issues)} STORY MISMATCH(ES):")
        for issue in issues:
            print(" -", issue)
        return 1

    print("story-conformance clean (names, axes, and stored controls)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
