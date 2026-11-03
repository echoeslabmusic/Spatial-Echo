// SpatialPresetCatalog: Catalog of spatial preset definitions with motion LFO and routing configurations.

#pragma once

#include <JuceHeader.h>
#include "../../SpatialParamIDs.h"
#include "../../DSP/SpatialMotionLfo.h"
#include <array>
#include <cmath>

namespace SpatialPresetCatalog
{
constexpr int kRoutingAfterEffects = 0;
constexpr int kRoutingBeforeEffects = 1;
// SpatialRestart choices used by the catalog. Note (index 2) restarts on
// incoming MIDI, which only the standalone app receives, so no preset uses it.
constexpr int kRetriggerFree = 0;
constexpr int kRetriggerTransport = 1;
constexpr int kRetriggerRandom = 3;
constexpr int kMenuItemsPerColumn = 14;

// One spatial preset: binaural width/crossover, routing, and motion settings.
struct Definition
{
    const char* displayName;
    const char* usage;
    float width;
    float crossoverHz;
    int routingMode;
    int motionPattern;       // 0=Still, 1=Orbit, 2=Pendulum, 3=Figure8, 4=Breathe, 5=Wander
    float motionRateHz;      // 0.01..8 Hz
    bool motionSync;         // tempo sync on/off
    int motionDivision;      // standard note division when synced (0-16)
    float motionDepth;       // 0..100%
    int motionRetrig;        // 0=Free, 1=Transport, 2=Note, 3=Random
    int motionChoreo;        // 0=Unison, 1=Constellation, 2=Cascade
    float motionSpread;      // 0..100%
    int motionLongDivision;  // 0=Off, 1=2/1, 2=4/1, 3=8/1, 4=16/1
    // Appended compatibility field; every preset explicitly clears it below.
    int motionLongVariantDivision;
};

struct Snapshot
{
    float width = 0.5f;
    float crossoverHz = 150.0f;
    int routingMode = kRoutingAfterEffects;
    int motionPattern = 0;  // Still (no motion)
    float motionRateHz = SpatialIDs::motion::kRateDefaultHz;
    bool motionSync = false;
    int motionDivision = SpatialIDs::motion::kDivisionDefault;
    int motionLongDivision = SpatialIDs::motion::kLongDivisionDefault;
    int motionLongVariantDivision = SpatialIDs::motion::kLongVariantDivisionDefault;
    float motionDepth = SpatialIDs::motion::kDepthDefault;
    int motionRetrig = SpatialIDs::motion::kRetrigDefault;
    int motionChoreo = SpatialIDs::motion::kChoreoDefault;
    float motionSpread = SpatialIDs::motion::kSpreadDefault;
};

inline constexpr std::array<const char*, 13> kParameterIds = {{
    SpatialIDs::binaural::kWidth,
    SpatialIDs::binaural::kCrossover,
    SpatialIDs::routing::kSpatialRouting,
    SpatialIDs::motion::kPattern,
    SpatialIDs::motion::kRate,
    SpatialIDs::motion::kSync,
    SpatialIDs::motion::kDivision,
    SpatialIDs::motion::kDepth,
    SpatialIDs::motion::kRetrig,
    SpatialIDs::motion::kChoreo,
    SpatialIDs::motion::kSpread,
    SpatialIDs::motion::kLongDivision,
    SpatialIDs::motion::kLongVariantDivision,
}};

// The 28 spatial presets.
// Mapping:  Pattern 0=Still, 1=Orbit, 2=Pendulum, 3=Figure8, 4=Breathe, 5=Wander
//
// NOTE: HeadPositionMode (per-head vs linked) is NOT part of kParameterIds, so MotionChoreo
// and MotionSpread values here are intentionally kept at defaults (0/0). Choreography is only
// audible once HeadPositionMode is set to per-head separately (e.g., via the PER-HEAD toggle).
// All presets leave Choreo/Spread at zero; the choreography layer is not reachable from these
// catalog presets alone.
inline constexpr std::array<Definition, 28> kDefinitions = {{
    {"Neutral", "Matches the startup spatial state with no motion. Use as the unmodified reference.", 0.50f, 150.0f,
     kRoutingAfterEffects, 0, 0.35f, false, 3, 45.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Narrow Field", "Narrows spatial width with no motion. Use where movement would distract.", 0.22f, 150.0f,
     kRoutingAfterEffects, 0, 0.35f, false, 3, 0.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Shallow Orbit 0.16 Hz",
     "Free-running Orbit at 0.16 Hz and 36% depth, after effects. Use for restrained continuous movement.", 0.22f,
     240.0f, kRoutingAfterEffects, 1, 0.16f, false, 3, 36.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Low-End Half-Note Orbit",
     "Transport-locked half-note Orbit at 50% depth, after effects, with a 240 Hz crossover. Use when lower frequencies should move less.",
     0.24f, 240.0f, kRoutingAfterEffects, 1, 0.35f, true, 1, 50.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Half-Triplet Pendulum",
     "Transport-locked half-note-triplet Pendulum at 47% depth, after effects. Use for a restrained triplet swing.",
     0.22f, 230.0f, kRoutingAfterEffects, 2, 0.35f, true, 7, 47.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Orbit 0.18 Hz",
     "Free-running Orbit at 0.18 Hz and 53% depth, after effects. Use for moderate continuous movement.", 0.24f, 170.0f,
     kRoutingAfterEffects, 1, 0.18f, false, 3, 53.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Wide Quarter-Note Orbit",
     "Transport-locked quarter-note Orbit at 78% depth, after effects. Use for strong regular movement.", 0.20f, 150.0f,
     kRoutingAfterEffects, 1, 0.35f, true, 2, 78.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Quarter-Triplet Pendulum",
     "Transport-locked quarter-note-triplet Pendulum at 75% depth, after effects. Use with triplet-feel material.",
     0.20f, 180.0f, kRoutingAfterEffects, 2, 0.35f, true, 8, 75.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Wide Orbit 0.05 Hz", "Free-running Orbit at 0.05 Hz and 67% depth, after effects. Use behind sustained sources.",
     0.22f, 160.0f, kRoutingAfterEffects, 1, 0.05f, false, 3, 67.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Broad Half-Note Orbit",
     "Transport-locked half-note Orbit at 69% depth, after effects. Use for broad tempo-linked movement.", 0.21f,
     160.0f, kRoutingAfterEffects, 1, 0.35f, true, 1, 69.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Orbit 0.22 Hz", "Free-running Orbit at 0.22 Hz and 49% depth, after effects. Use for moderate unsynced movement.",
     0.23f, 180.0f, kRoutingAfterEffects, 1, 0.22f, false, 3, 49.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Subtle Half-Note Orbit",
     "Transport-locked half-note Orbit at 39% depth, after effects, with a 300 Hz crossover. Use for restrained synced movement.",
     0.21f, 300.0f, kRoutingAfterEffects, 1, 0.35f, true, 1, 39.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Light Whole-Note Breathe",
     "Transport-locked whole-note Breathe cycle at 46% depth, after effects. Use for light near/far movement.", 0.24f,
     130.0f, kRoutingAfterEffects, 4, 0.35f, true, 0, 46.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Shallow Orbit 0.11 Hz",
     "Free-running Orbit at 0.11 Hz and 42% depth, after effects. Use for slow restrained motion.", 0.23f, 150.0f,
     kRoutingAfterEffects, 1, 0.11f, false, 3, 42.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Pre-FX Wander 0.22 Hz",
     "Free-running Wander at 0.22 Hz and 53% depth before effects. Use when irregular movement should feed the echo and reverb tails.",
     0.22f, 150.0f, kRoutingBeforeEffects, 5, 0.22f, false, 3, 53.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Orbit 0.10 Hz", "Free-running Orbit at 0.10 Hz and 49% depth, after effects. Use for slow moderate movement.",
     0.24f, 140.0f, kRoutingAfterEffects, 1, 0.10f, false, 3, 49.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Deep Wander 0.30 Hz",
     "Free-running Wander at 0.30 Hz and 72% depth, after effects. Use for active irregular movement.", 0.25f, 140.0f,
     kRoutingAfterEffects, 5, 0.30f, false, 3, 72.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Deep Wander 0.16 Hz",
     "Free-running Wander at 0.16 Hz and 81% depth, after effects. Use for deep irregular movement.", 0.22f, 320.0f,
     kRoutingAfterEffects, 5, 0.16f, false, 3, 81.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Deep Orbit 0.35 Hz", "Free-running Orbit at 0.35 Hz and 92% depth. Use for obvious cyclic movement.", 0.20f,
     170.0f, kRoutingAfterEffects, 1, 0.35f, false, 3, 92.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Figure 8 3.60 Hz", "Free-running Figure 8 at 3.6 Hz and 58% depth. Use as a fast motion texture.", 0.22f, 200.0f,
     kRoutingAfterEffects, 3, 3.60f, false, 3, 58.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Full Wander 1.20 Hz",
     "Free-running Wander at 1.2 Hz and full depth, after effects. Use for rapid irregular movement.", 0.18f, 180.0f,
     kRoutingAfterEffects, 5, 1.20f, false, 3, 100.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Pre-FX Whole-Note Orbit",
     "Transport-locked whole-note Orbit at full depth before effects. Use when a slow orbit should feed the echo and reverb tails.",
     0.20f, 160.0f, kRoutingBeforeEffects, 1, 0.35f, true, 0, 100.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Pre-FX Half-Note Orbit",
     "Transport-locked half-note Orbit at 85% depth before effects. Use when a faster orbit should feed the echo and reverb tails.",
     0.20f, 160.0f, kRoutingBeforeEffects, 1, 0.35f, true, 1, 85.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Deep Half-Note Orbit",
     "Transport-locked half-note Orbit at 67% depth, after effects. Use for deep tempo-linked movement.", 0.21f, 170.0f,
     kRoutingAfterEffects, 1, 0.35f, true, 1, 67.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Deep Whole-Note Breathe",
     "Transport-locked whole-note Breathe cycle at 61% depth, after effects. Use for deep near/far movement.", 0.22f,
     130.0f, kRoutingAfterEffects, 4, 0.35f, true, 0, 61.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Dotted-Half Orbit",
     "Transport-locked dotted-half-note Orbit at 58% depth, after effects. Use for a long sync cycle that crosses the bar grid.",
     0.20f, 300.0f, kRoutingAfterEffects, 1, 0.35f, true, 12, 58.0f, kRetriggerTransport, 0, 0.0f, 0, 0},
    {"Quarter-Note Figure 8",
     "Tempo-synced quarter-note Figure 8 at full depth, after effects. Use for strong figure-eight movement.", 0.18f,
     210.0f, kRoutingAfterEffects, 3, 0.35f, true, 2, 100.0f, kRetriggerFree, 0, 0.0f, 0, 0},
    {"Random-Phase Orbit",
     "Free-running 2.4 Hz Orbit with a randomized phase each cycle. Use for abrupt discontinuous movement.", 0.22f,
     170.0f, kRoutingAfterEffects, 1, 2.40f, false, 3, 100.0f, kRetriggerRandom, 0, 0.0f, 0, 0},
}};
static_assert(kDefinitions.size() == 2 * kMenuItemsPerColumn, "Spatial preset menu must contain two equal columns.");

inline bool approximatelyEqual(float a, float b, float tolerance)
{
    return std::abs(a - b) <= tolerance;
}

inline float getFloatParam(const juce::AudioProcessorValueTreeState& valueTreeState, const char* id, float fallback)
{
    if (auto* raw = valueTreeState.getRawParameterValue(id))
        return raw->load();
    return fallback;
}

inline int getChoiceParam(const juce::AudioProcessorValueTreeState& valueTreeState, const char* id, int fallback)
{
    return juce::roundToInt(getFloatParam(valueTreeState, id, static_cast<float>(fallback)));
}

inline bool getBoolParam(const juce::AudioProcessorValueTreeState& valueTreeState, const char* id, bool fallback)
{
    return getFloatParam(valueTreeState, id, fallback ? 1.0f : 0.0f) > 0.5f;
}

inline Snapshot readSnapshot(const juce::AudioProcessorValueTreeState& valueTreeState)
{
    Snapshot s;
    s.width = getFloatParam(valueTreeState, SpatialIDs::binaural::kWidth, 0.5f);
    s.crossoverHz = getFloatParam(valueTreeState, SpatialIDs::binaural::kCrossover, 150.0f);
    s.routingMode = getChoiceParam(valueTreeState, SpatialIDs::routing::kSpatialRouting, kRoutingAfterEffects);
    s.motionPattern = getChoiceParam(valueTreeState, SpatialIDs::motion::kPattern, 0);
    s.motionRateHz = getFloatParam(valueTreeState, SpatialIDs::motion::kRate, SpatialIDs::motion::kRateDefaultHz);
    s.motionSync = getBoolParam(valueTreeState, SpatialIDs::motion::kSync, false);
    s.motionDivision =
        getChoiceParam(valueTreeState, SpatialIDs::motion::kDivision, SpatialIDs::motion::kDivisionDefault);
    s.motionLongDivision =
        getChoiceParam(valueTreeState, SpatialIDs::motion::kLongDivision, SpatialIDs::motion::kLongDivisionDefault);
    s.motionLongVariantDivision = getChoiceParam(valueTreeState, SpatialIDs::motion::kLongVariantDivision,
                                                 SpatialIDs::motion::kLongVariantDivisionDefault);
    s.motionDepth = getFloatParam(valueTreeState, SpatialIDs::motion::kDepth, SpatialIDs::motion::kDepthDefault);
    s.motionRetrig = getChoiceParam(valueTreeState, SpatialIDs::motion::kRetrig, SpatialIDs::motion::kRetrigDefault);
    s.motionChoreo = getChoiceParam(valueTreeState, SpatialIDs::motion::kChoreo, SpatialIDs::motion::kChoreoDefault);
    s.motionSpread = getFloatParam(valueTreeState, SpatialIDs::motion::kSpread, SpatialIDs::motion::kSpreadDefault);
    return s;
}

inline int findMatchingPresetIndex(const Snapshot& s)
{
    for (size_t i = 0; i < kDefinitions.size(); ++i)
    {
        const auto& preset = kDefinitions[i];
        bool matches = true;
        matches &= approximatelyEqual(s.width, preset.width, 0.01f);
        matches &= approximatelyEqual(s.crossoverHz, preset.crossoverHz, 0.51f);
        matches &= (s.routingMode == preset.routingMode);
        matches &= (s.motionPattern == preset.motionPattern);
        matches &= approximatelyEqual(s.motionRateHz, preset.motionRateHz, 0.02f);
        matches &= (s.motionSync == preset.motionSync);
        if (preset.motionSync)
        {
            matches &= (s.motionDivision == preset.motionDivision);
            matches &= (s.motionLongDivision == preset.motionLongDivision);
            matches &= (s.motionLongVariantDivision == preset.motionLongVariantDivision);
        }
        matches &= approximatelyEqual(s.motionDepth, preset.motionDepth, 1.0f);
        matches &= (s.motionRetrig == preset.motionRetrig);
        matches &= (s.motionChoreo == preset.motionChoreo);
        matches &= approximatelyEqual(s.motionSpread, preset.motionSpread, 0.5f);

        if (matches)
            return static_cast<int>(i);
    }

    return -1;
}

inline void setParameterValue(juce::AudioProcessorValueTreeState& valueTreeState, const char* parameterId, float value,
                              bool withGesture)
{
    if (auto* param = valueTreeState.getParameter(parameterId))
    {
        if (withGesture)
            param->beginChangeGesture();
        param->setValueNotifyingHost(param->convertTo0to1(value));
        if (withGesture)
            param->endChangeGesture();
    }
}

inline bool applyPresetToParameters(juce::AudioProcessorValueTreeState& valueTreeState, int presetIndex,
                                    bool withGesture)
{
    if (presetIndex < 0 || presetIndex >= static_cast<int>(kDefinitions.size()))
        return false;

    const auto& preset = kDefinitions[static_cast<size_t>(presetIndex)];
    setParameterValue(valueTreeState, SpatialIDs::binaural::kWidth, preset.width, withGesture);
    setParameterValue(valueTreeState, SpatialIDs::binaural::kCrossover, preset.crossoverHz, withGesture);
    setParameterValue(valueTreeState, SpatialIDs::routing::kSpatialRouting, static_cast<float>(preset.routingMode),
                      withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kPattern, static_cast<float>(preset.motionPattern),
                      withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kRate, preset.motionRateHz, withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kSync, preset.motionSync ? 1.0f : 0.0f, withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kDivision, static_cast<float>(preset.motionDivision),
                      withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kLongDivision, static_cast<float>(preset.motionLongDivision),
                      withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kLongVariantDivision,
                      static_cast<float>(preset.motionLongVariantDivision), withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kDepth, preset.motionDepth, withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kRetrig, static_cast<float>(preset.motionRetrig),
                      withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kChoreo, static_cast<float>(preset.motionChoreo),
                      withGesture);
    setParameterValue(valueTreeState, SpatialIDs::motion::kSpread, preset.motionSpread, withGesture);
    return true;
}
}  // namespace SpatialPresetCatalog
