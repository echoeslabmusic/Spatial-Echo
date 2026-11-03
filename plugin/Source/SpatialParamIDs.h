#pragma once

// =============================================================================
// SpatialParamIDs.h - single source of truth for every spatial parameter ID.
//
// IDs, types, ranges, and defaults declared here are the contract between the
// DSP engine and the UI.
//
// Rules:
//   - No spatial param-ID string literals in plug-in code outside this header.
//     DSP, processor, preset, and UI code all reference these constants. Only
//     the generated factory-preset table and the tests' independent
//     expectations spell the IDs out.
//   - This header stays JUCE-free so both the engine and the UI can include it
//     from anywhere.
// =============================================================================

namespace SpatialIDs
{

// -----------------------------------------------------------------------------
// Pattern-first motion schema (8 params)
// -----------------------------------------------------------------------------

namespace motion
{
// PATTERN - the pattern IS the parameter; the engine renders it as a true
// trajectory recipe. "Still" means motion is off; any other pattern means
// motion is on.
inline constexpr const char* kPattern = "SpatialPattern";
inline constexpr const char* kPatternChoices[] = {"Still", "Orbit", "Pendulum", "Figure-8", "Breathe", "Wander"};
inline constexpr int kPatternCount = 6;
inline constexpr int kPatternDefault = 0;  // Still
enum class Pattern : int
{
    Still = 0,
    Orbit,
    Pendulum,
    Figure8,
    Breathe,
    Wander
};

// RATE - free-running rate in Hz, log-skewed (centre ~= geometric mean
// sqrt(0.01 * 8) ~= 0.28 Hz).
inline constexpr const char* kRate = "SpatialRate";
inline constexpr float kRateMinHz = 0.01f;
inline constexpr float kRateMaxHz = 8.0f;
inline constexpr float kRateDefaultHz = 0.35f;
inline constexpr float kSyncedRateMaxHz = 16.0f;  // established fast-motion safety ceiling

// SYNC - tempo sync on/off; when on, DIV supplies the period.
inline constexpr const char* kSync = "SpatialSync";
inline constexpr bool kSyncDefault = false;

// DIV - original note-division parameter. Its 17 choices and normalized
// mapping are compatibility-sensitive host-automation state.
inline constexpr const char* kDivision = "SpatialDivision";
inline constexpr const char* kDivisionChoices[] = {
    "1/1",  "1/2",  "1/4",  "1/8",   "1/16",  "1/32", "1/64",  // Straight
    "1/2T", "1/4T", "1/8T", "1/16T", "1/32T",                  // Triplets
    "1/2D", "1/4D", "1/8D", "1/16D", "1/32D"                   // Dotted
};
inline constexpr int kDivisionCount = 17;
static_assert(static_cast<int>(sizeof(kDivisionChoices) / sizeof(kDivisionChoices[0])) == kDivisionCount);
inline constexpr int kDivisionDefault = 3;  // 1/8

// LONG DIV - compatibility-safe extension for straight motion periods
// longer than 1/1. Off reveals SpatialDivision; indices 1-4 override it.
// Its five-choice normalized domain is frozen at i/4.
inline constexpr const char* kLongDivision = "SpatialLongDivision";
inline constexpr const char* kLongDivisionChoices[] = {"Off", "2/1", "4/1", "8/1", "16/1"};
inline constexpr int kLongDivisionCount = 5;
inline constexpr int kLongDivisionDefault = 0;
static_assert(static_cast<int>(sizeof(kLongDivisionChoices) / sizeof(kLongDivisionChoices[0])) == kLongDivisionCount);

// LONG VARIANT DIV - second append-only extension for long triplet and
// dotted periods. Keeping these values in their own v3 parameter preserves
// every normalized automation anchor in SpatialLongDivision.
inline constexpr const char* kLongVariantDivision = "SpatialLongVariantDivision";
inline constexpr const char* kLongVariantDivisionChoices[] = {"Off",  "2/1T", "4/1T", "8/1T", "16/1T",
                                                              "2/1D", "4/1D", "8/1D", "16/1D"};
inline constexpr int kLongVariantDivisionCount = 9;
inline constexpr int kLongVariantDivisionDefault = 0;
// Item positions in the combined division picker: the base divisions, then the
// straight long divisions, then the triplet and dotted long divisions. The
// static_asserts below keep them consistent with the three parameters.
inline constexpr int kStraightLongMenuStart = 17;
inline constexpr int kVariantLongMenuStart = 21;
inline constexpr int kDivisionMenuCount = 29;
static_assert(static_cast<int>(sizeof(kLongVariantDivisionChoices) / sizeof(kLongVariantDivisionChoices[0])) ==
              kLongVariantDivisionCount);
static_assert(kStraightLongMenuStart == kDivisionCount);
static_assert(kVariantLongMenuStart == kStraightLongMenuStart + kLongDivisionCount - 1);
static_assert(kDivisionMenuCount == kVariantLongMenuStart + kLongVariantDivisionCount - 1);

inline constexpr int toDivisionMenuIndex(int standardIndex, int longIndex, int longVariantIndex) noexcept
{
    if (longVariantIndex > 0)
        return kVariantLongMenuStart + longVariantIndex - 1;
    if (longIndex > 0)
        return kStraightLongMenuStart + longIndex - 1;
    return standardIndex;
}

inline constexpr int longDivisionFromMenuIndex(int menuIndex) noexcept
{
    return menuIndex >= kStraightLongMenuStart && menuIndex < kVariantLongMenuStart
               ? menuIndex - kStraightLongMenuStart + 1
               : kLongDivisionDefault;
}

inline constexpr int longVariantDivisionFromMenuIndex(int menuIndex) noexcept
{
    return menuIndex >= kVariantLongMenuStart && menuIndex < kDivisionMenuCount ? menuIndex - kVariantLongMenuStart + 1
                                                                                : kLongVariantDivisionDefault;
}

// DEPTH - one macro scaling the selected recipe's modulation amplitudes.
// 0 % => exactly zero offsets for every pattern.
inline constexpr const char* kDepth = "SpatialDepth";
inline constexpr float kDepthMin = 0.0f;
inline constexpr float kDepthMax = 100.0f;
inline constexpr float kDepthDefault = 45.0f;

// RETRIG - phase retrigger source. Note retriggers only in the Standalone
// build, the only build that receives MIDI.
inline constexpr const char* kRetrig = "SpatialRestart";
inline constexpr const char* kRetrigChoices[] = {"Free", "Transport", "Note", "Random"};
inline constexpr int kRetrigCount = 4;
inline constexpr int kRetrigDefault = 0;  // Free

// CHOREO - per-tap phase choreography (Unison = 0 offset, Constellation =
// spread-scaled 120 deg offsets, Cascade = delay-ratio stagger).
inline constexpr const char* kChoreo = "SpatialChoreography";
inline constexpr const char* kChoreoChoices[] = {"Unison", "Constellation", "Cascade"};
inline constexpr int kChoreoCount = 3;
inline constexpr int kChoreoDefault = 0;  // Unison

// SPREAD - choreography spread amount.
inline constexpr const char* kSpread = "SpatialSpread";
inline constexpr float kSpreadMin = 0.0f;
inline constexpr float kSpreadMax = 100.0f;
inline constexpr float kSpreadDefault = 0.0f;

// Host display strings. The visual surface keeps compact labels inside the
// SPATIAL MOTION section; standalone host/accessibility names are explicit.
inline constexpr const char* kPatternDisplay = "Spatial Pattern";
inline constexpr const char* kRateDisplay = "Spatial Rate";
inline constexpr const char* kSyncDisplay = "Spatial Sync";
inline constexpr const char* kDivisionDisplay = "Spatial Division";
inline constexpr const char* kLongDivisionDisplay = "Spatial Long Division";
inline constexpr const char* kLongVariantDivisionDisplay = "Spatial Long Variant Division";
inline constexpr const char* kDepthDisplay = "Spatial Depth";
inline constexpr const char* kRetrigDisplay = "Spatial Restart";
inline constexpr const char* kChoreoDisplay = "Spatial Choreography";
inline constexpr const char* kSpreadDisplay = "Spatial Spread";
}  // namespace motion

// -----------------------------------------------------------------------------
// Binaural position, tap and routing parameters
// -----------------------------------------------------------------------------

namespace binaural
{
inline constexpr const char* kEnabled = "SpatialEnabled";
inline constexpr const char* kAzimuth = "SpatialAzimuth";
inline constexpr const char* kElevation = "SpatialElevation";
inline constexpr const char* kDistance = "SpatialDistance";
inline constexpr const char* kLockAzimuth = "SpatialLockAzimuth";
inline constexpr const char* kLockElevation = "SpatialLockElevation";
inline constexpr const char* kLockDistance = "SpatialLockDistance";
inline constexpr const char* kWidth = "SpatialSourceWidth";
inline constexpr const char* kWetMix = "SpatialMix";
inline constexpr const char* kCrossover = "SpatialCrossover";
inline constexpr const char* kQuality = "SpatialQuality";
}  // namespace binaural

namespace taps
{
inline constexpr const char* kPositionMode = "HeadPositionMode";

inline constexpr const char* kHead1Azimuth = "Head1Azimuth";
inline constexpr const char* kHead1Elevation = "Head1Elevation";
inline constexpr const char* kHead1Distance = "Head1Distance";
inline constexpr const char* kHead2Azimuth = "Head2Azimuth";
inline constexpr const char* kHead2Elevation = "Head2Elevation";
inline constexpr const char* kHead2Distance = "Head2Distance";
inline constexpr const char* kHead3Azimuth = "Head3Azimuth";
inline constexpr const char* kHead3Elevation = "Head3Elevation";
inline constexpr const char* kHead3Distance = "Head3Distance";
}  // namespace taps

namespace routing
{
inline constexpr const char* kSpatialRouting = "SpatialRouting";
}  // namespace routing

}  // namespace SpatialIDs
