// ReverbPresetLibrary: 8 spring reverb presets and 10 cloud reverb presets in the reverb macro schema.

#pragma once

#include <array>
#include <cstddef>

struct ReverbPresetDefinition
{
    const char* name;
    const char* description;
    float size;       // 0-1 UI space
    float decay;      // 0-1 UI space
    float tone;       // 0-1, 0=dark, 1=bright
    float mod;        // 0-1, modulation depth
    float character;  // 0-1, spring: spring balance; cloud: density
    float preDelay;   // 0-1 (0-200ms)
    float width;      // 0-1 stereo width
};

// Spring reverb presets in macro schema. Mapping from the engine-native values:
//   size, decay, preDelay: unchanged (1:1)
//   tone = clamp01(1 − nativeDamping / 1.128); neutral 0.5 ↔ damping 0.564
//   mod = nativeModDepth
//   character (spring) = nativeSpringBalance
//   width (spring) = 0.0 for mono-intent; else 0.4-0.6 based on imaging goal
//
// Columns: size, decay, tone, mod, character, preDelay, width
inline constexpr std::array<ReverbPresetDefinition, 8> kReverbPresets = {
    {{"Tight Spring", "Short guitar-amp spring with snappy attack.",
      // Native: size=0.12, decay=0.25, damping=0.18, modDepth=0.02, springBalance=0.30
      // Mapping: tone = 1 - (0.18/1.128) = 0.84; mod = 0.02; character = 0.30; width = 0.0 (snappy, no stereo intent)
      0.12f, 0.25f, 0.84f, 0.02f, 0.30f, 0.00f, 0.0f},
     {"Percussive Slap", "Ultra-short punchy reflections for drums.",
      // Native: size=0.08, decay=0.12, damping=0.30, modDepth=0.01, springBalance=0.25
      // Mapping: tone = 1 - (0.30/1.128) = 0.73; mod = 0.01; character = 0.25; width = 0.0 (punchy/percussive, no
      // stereo)
      0.08f, 0.12f, 0.73f, 0.01f, 0.25f, 0.02f, 0.0f},
     {"Surf Drip", "Classic bright spring chirp for surf guitar.",
      // Native: size=0.40, decay=0.62, damping=0.05, modDepth=0.06, springBalance=0.20
      // Mapping: tone = 1 - (0.05/1.128) = 0.96 (very bright); mod = 0.06; width = 0.0 (classic mono intent)
      // Character 0.35 rather than the mapped 0.20: the mechanical map loses
      // the native scatter=0.55 "drip"; 0.35 restores junction ring while keeping
      // the bright spring path.
      0.40f, 0.62f, 0.96f, 0.06f, 0.35f, 0.00f, 0.0f},
     {"Bright Spring", "Forward and present with clear high-end.",
      // Native: size=0.30, decay=0.48, damping=0.06, modDepth=0.04, springBalance=0.32
      // Mapping: tone = 1 - (0.06/1.128) = 0.95 (very bright); mod = 0.04; character = 0.32; width = 0.0 (modern clean,
      // mono)
      0.30f, 0.48f, 0.95f, 0.04f, 0.32f, 0.03f, 0.0f},
     {"Vintage Tank", "Balanced neutral workhorse spring reverb.",
      // Native: size=0.48, decay=0.58, damping=0.30, modDepth=0.05, springBalance=0.55
      // Mapping: tone = 1 - (0.30/1.128) = 0.73; mod = 0.05; character = 0.55; width = 0.0 (neutral, mono intent)
      0.48f, 0.58f, 0.73f, 0.05f, 0.55f, 0.03f, 0.0f},
     {"Vocal Spring", "Smooth and flattering with pre-delay for clarity.",
      // Native: size=0.42, decay=0.55, damping=0.38, modDepth=0.03, springBalance=0.60, preDelay=0.18
      // Mapping: tone = 1 - (0.38/1.128) = 0.66 (warm); mod = 0.03; character = 0.60; width = 0.0 (vocal, no stereo)
      0.42f, 0.55f, 0.66f, 0.03f, 0.60f, 0.18f, 0.0f},
     {"Wide Spring", "Spacious stereo spring via Haas-zone pre-delay.",
      // Native: size=0.50, decay=0.55, damping=0.15, modDepth=0.07, springBalance=0.45, preDelay=0.12, prefersStereo=true
      // Mapping: tone = 1 - (0.15/1.128) = 0.87 (bright); mod = 0.07; character = 0.45; width = 0.5 (stereo imaging
      // goal)
      0.50f, 0.55f, 0.87f, 0.07f, 0.45f, 0.12f, 0.5f},
     {"Lo-Fi Coil", "Heavily colored dark character with saturation.",
      // Native: size=0.22, decay=0.42, damping=0.62, modDepth=0.08, springBalance=0.75
      // Tone 0.20 rather than the mapped 0.45: the damping-only mapping loses
      // the native explicit 4.5 kHz high-cut (measured brightness moves UP, the
      // wrong direction for lo-fi); 0.20 restores the dark coil voicing.
      0.22f, 0.42f, 0.20f, 0.08f, 0.75f, 0.00f, 0.0f}}};

inline constexpr size_t kNumReverbPresets = kReverbPresets.size();

// ─── Cloud (algorithmic) reverb presets ────────────────────────────
// CloudSeed engine in macro schema. Mapping:
//   size, decay, preDelay: unchanged (1:1)
//   tone = clamp01(1 − nativeDamping / 1.128)
//   mod = nativeModDepth
//   character = clamp01((nativeDiffusion − 0.15) / 0.70)
//   width = nativeStereoWidth
struct CloudReverbPresetDefinition
{
    const char* name;
    const char* description;
    float size;       // 0-1 UI space
    float decay;      // 0-1 UI space
    float tone;       // 0-1, 0=dark, 1=bright
    float mod;        // 0-1, modulation depth
    float character;  // 0-1, cloud: bloom/density
    float preDelay;   // 0-1 (0-200ms)
    float width;      // 0-1 stereo width
};

inline constexpr std::array<CloudReverbPresetDefinition, 10> kCloudReverbPresets = {
    {{"Small Room", "Tight natural room with short decay.",
      // Native: size=0.15, decay=0.20, damping=0.35, diffusion=0.30, modDepth=0.05, stereoWidth=0.40
      // Mapping: tone = 1 - (0.35/1.128) = 0.69; mod = 0.05; character = (0.30-0.15)/0.70 = 0.21; width = 0.40
      0.15f, 0.20f, 0.69f, 0.05f, 0.21f, 0.01f, 0.40f},
     {"Dark Plate", "Warm dense plate reverb with rolled-off highs.",
      // Native: size=0.40, decay=0.55, damping=0.65, diffusion=0.75, modDepth=0.08, stereoWidth=0.55
      // Tone 0.31: averages the damping-implied tone (0.42) with the native
      // explicit 5 kHz high-cut's implied tone (0.19).
      0.40f, 0.55f, 0.31f, 0.08f, 0.86f, 0.02f, 0.55f},
     {"Bright Hall", "Open shimmering concert hall with clear high-end.",
      // Native: size=0.70, decay=0.65, damping=0.10, diffusion=0.60, modDepth=0.12, stereoWidth=0.70
      // Mapping: tone = 1 - (0.10/1.128) = 0.91 (bright); mod = 0.12; character = (0.60-0.15)/0.70 = 0.64; width = 0.70
      0.70f, 0.65f, 0.91f, 0.12f, 0.64f, 0.08f, 0.70f},
     {"Cathedral", "Huge reverberant space with massive decay.",
      // Native: size=0.90, decay=0.80, damping=0.25, diffusion=0.80, modDepth=0.15, preDelay=0.15, stereoWidth=0.75
      // Mapping: tone = 1 - (0.25/1.128) = 0.78; mod = 0.15; character = (0.80-0.15)/0.70 = 0.93; width = 0.75
      0.90f, 0.80f, 0.78f, 0.15f, 0.93f, 0.15f, 0.75f},
     {"Ambient Wash", "Medium space with heavy modulation for background texture.",
      // Native: size=0.55, decay=0.60, damping=0.30, diffusion=0.65, modDepth=0.35, stereoWidth=0.60
      // Mapping: tone = 1 - (0.30/1.128) = 0.73; mod = 0.35; character = (0.65-0.15)/0.70 = 0.71
      // Width 0.80 rather than the mapped 0.60: the M/S width law measures
      // narrower than the native seed width (corr 0.12 vs 0.26); 0.80
      // restores the wash's spread.
      0.55f, 0.60f, 0.73f, 0.35f, 0.71f, 0.04f, 0.80f},
     {"Gated Verb", "Short punchy reverb with sharp cutoff.",
      // Native: size=0.25, decay=0.12, damping=0.50, diffusion=0.20, modDepth=0.03, stereoWidth=0.50
      // Mapping: tone = 1 - (0.50/1.128) = 0.56; mod = 0.03; character = (0.20-0.15)/0.70 = 0.07; width = 0.50
      0.25f, 0.12f, 0.56f, 0.03f, 0.07f, 0.00f, 0.50f},
     {"Modulated Space", "Medium room with deep chorus-like modulation.",
      // Native: size=0.45, decay=0.50, damping=0.20, diffusion=0.55, modDepth=0.45, stereoWidth=0.65
      // Mapping: tone = 1 - (0.20/1.128) = 0.82 (bright); mod = 0.45; character = (0.55-0.15)/0.70 = 0.57; width = 0.65
      0.45f, 0.50f, 0.82f, 0.45f, 0.57f, 0.03f, 0.65f},
     {"Lo-Fi Cloud", "Dark filtered reverb; low character gives a grainy, sparse texture.",
      // Native: size=0.35, decay=0.45, damping=0.70, diffusion=0.40, modDepth=0.10, stereoWidth=0.30
      // Tone 0.21: averages the damping-implied tone (0.38) with the native
      // explicit 4 kHz high-cut's implied tone (0.05).
      0.35f, 0.45f, 0.21f, 0.10f, 0.36f, 0.01f, 0.30f},
     {"Infinite Shimmer", "Extreme decay with heavy modulation for ethereal pads.",
      // Native: size=0.80, decay=0.95, damping=0.15, diffusion=0.90, modDepth=0.40, stereoWidth=0.80
      // Mapping: tone = 1 - (0.15/1.128) = 0.87 (bright); mod = 0.40; character = (0.90-0.15)/0.70 = 1.07 → clamp
      // = 1.0; width = 0.80
      0.80f, 0.95f, 0.87f, 0.40f, 1.0f, 0.06f, 0.80f},
     {"Ethereal Pad", "Maximum diffusion with long decay for pad-like wash.",
      // Native: size=0.75, decay=0.85, damping=0.20, diffusion=0.85, modDepth=0.20, preDelay=0.10, stereoWidth=0.85
      // Mapping: tone = 1 - (0.20/1.128) = 0.82 (bright); mod = 0.20; character = (0.85-0.15)/0.70 = 1.0; width = 0.85
      0.75f, 0.85f, 0.82f, 0.20f, 1.0f, 0.10f, 0.85f}}};

inline constexpr size_t kNumCloudReverbPresets = kCloudReverbPresets.size();
