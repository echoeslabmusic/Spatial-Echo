// HeadRatioConstants: Playback head timing ratio presets (classic, golden, linear, etc.).

#pragma once

// Centralized constants for head timing ratios used across the codebase.
// These define the relationship between playback head delay times.
// ============================================================================

namespace HeadRatios
{
// Base delay time for Head 1 (reference point)
constexpr float HEAD1_BASE_DELAY_MS = 167.0f;

// Classic (1:1.901:2.751) is an artistic choice; no documented hardware
// source is known for these ratios.
constexpr float CLASSIC_RATIO_2 = 1.901f;
constexpr float CLASSIC_RATIO_3 = 2.751f;

// Idealized (1:2:3) matches the documented Roland RE-201 head ratio.
constexpr float IDEALIZED_RATIO_2 = 2.0f;
constexpr float IDEALIZED_RATIO_3 = 3.0f;

// Golden uses the golden ratio φ: 1:φ:φ² (rounded constants below).
constexpr float GOLDEN_RATIO_2 = 1.618f;
constexpr float GOLDEN_RATIO_3 = 2.618f;

// Linear (1:1.5:2) is an artistic choice; no documented hardware source
// is known for these ratios.
constexpr float LINEAR_RATIO_2 = 1.5f;
constexpr float LINEAR_RATIO_3 = 2.0f;
}  // namespace HeadRatios
