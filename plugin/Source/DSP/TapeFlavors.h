// TapeFlavors: The five user-facing tape flavors. Each flavor is a fully tuned bundle over the internal tape params
//   (record-stage drive/sat/bias, wow&flutter, noise, condition, darkening), plus a per-flavor DRIVE law that scales the
//   bundle around its anchor. TapeFlavor + TapeDrive are the only tape-facing APVTS parameters; everything else in the
//   bundle is internal.
// Realtime-safety: pure constexpr-style data + arithmetic; no allocation.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

enum class TapeFlavor
{
    Clean,     // pristine mastering path (SM900/Broadcast basis), noise off
    Warm,      // studio standard (SM911 verbatim) - DEFAULT; anchors the default sound
    Hot,       // pushed machine (LPR35 basis driven harder), early breakup
    Cassette,  // consumer cassette (verbatim Cassette Lo-Fi bundle)
    Worn,      // degraded transport (verbatim Worn Tape bundle)

    NumFlavors
};

struct TapeFlavorInfo
{
    const char* name;
    const char* description;

    // Record-stage anchors (values at the DRIVE anchor position)
    float drive;
    float saturation;
    float bias;

    // Wow/flutter bundle
    float wowDepth;
    float flutterDepth;
    float wowRateScale;      // 0.5-2.0
    float flutterRateScale;  // 0.5-2.0

    // Noise bundle
    bool noiseEnabled;
    float hissLevel;
    float asperityLevel;
    float dropoutProbability;

    // Wear + response
    float condition;
    float scrapeFlutter;
    float lpfCutoffScale;  // drives per-flavor feedback darkening (D-2)

    // DRIVE-law slopes around the anchor (steeper = earlier breakup)
    float kDrive;
    float kSaturation;
    float kBias;
};

namespace TapeFlavors
{

// The DRIVE knob position at which every flavor reproduces its anchor bundle
// exactly. 0.45 is the TapeDrive default, so WARM at the anchor reproduces the
// default sound by construction.
inline constexpr float kDriveAnchor = 0.45f;
inline float sanitizeDriveKnob01(float driveKnob01) noexcept
{
    return std::clamp(std::isfinite(driveKnob01) ? driveKnob01 : kDriveAnchor, 0.0f, 1.0f);
}

inline constexpr std::array<float, 5> kTapeRecordMakeupDriveKnots{0.0f, 0.225f, 0.45f, 0.725f, 1.0f};
// The first-party saturator is normalized to unity at small signal. Keep the
// calibration table explicit so measured stage-owned corrections can be added
// without changing the user-facing flavor/drive law.
inline constexpr std::array<std::array<float, 5>, static_cast<int>(TapeFlavor::NumFlavors)> kTapeRecordMakeupDb{{
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},  // Clean
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},  // Warm
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},  // Hot
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},  // Cassette
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},  // Worn
}};

inline constexpr TapeFlavorInfo kFlavors[static_cast<int>(TapeFlavor::NumFlavors)] = {
    {"CLEAN", "Pristine mastering tape, transparent path", 0.35f, 0.80f, 0.75f, 0.12f, 0.10f, 1.0f, 1.0f, false, 0.00f,
     0.00f, 0.0f,
     // lpfScale 1.02, not SM900's 1.08: 1.08 brightens echo tails by about
     // 1-3 dB in the air band relative to the factory presets.
     0.03f, 0.05f, 1.02f, 0.30f, 0.25f, 0.15f},

    {"WARM", "Studio standard, balanced saturation", 0.45f, 0.65f, 0.60f, 0.25f, 0.18f, 1.0f, 1.0f, false, 0.12f, 0.03f,
     0.0f, 0.08f, 0.07f, 1.05f, 0.40f, 0.45f, 0.20f},

    {"HOT", "Driven warm machine, dub territory",
     // Anchor = dub-preset territory: the factory bank's users of this slot
     // (all dub/feedback presets) sit at drive/sat/bias ~= 0.60/0.35/0.30
     // with hiss on and moderate wear.
     0.60f, 0.35f, 0.30f, 0.45f, 0.35f, 0.85f, 1.00f, true, 0.20f, 0.10f, 0.002f, 0.35f, 0.15f, 0.97f, 0.50f, 0.55f,
     0.30f},

    {"CASSETTE", "Consumer cassette, audible hiss and wobble", 0.58f, 0.38f, 0.32f, 0.70f, 0.60f, 0.70f, 1.20f, true,
     0.65f, 0.25f, 0.005f, 0.55f, 0.25f, 0.92f, 0.55f, 0.60f, 0.35f},

    {"WORN", "Degraded tape, dropouts and heavy character", 0.68f, 0.22f, 0.10f, 0.90f, 0.80f, 0.60f, 1.30f, true,
     0.85f, 0.40f, 0.015f, 0.85f, 0.40f, 0.88f, 0.60f, 0.65f, 0.40f},
};

inline const TapeFlavorInfo& get(int flavorIndex)
{
    const int clamped = std::clamp(flavorIndex, 0, static_cast<int>(TapeFlavor::NumFlavors) - 1);
    return kFlavors[clamped];
}

inline float getTapeRecordMakeupDb(int flavorIndex, float driveKnob01) noexcept
{
    const int clampedFlavor = std::clamp(flavorIndex, 0, static_cast<int>(TapeFlavor::NumFlavors) - 1);
    const float clampedDrive = sanitizeDriveKnob01(driveKnob01);
    const auto& row = kTapeRecordMakeupDb[static_cast<size_t>(clampedFlavor)];

    if (clampedDrive <= kTapeRecordMakeupDriveKnots.front())
        return row.front();
    if (clampedDrive >= kTapeRecordMakeupDriveKnots.back())
        return row.back();

    for (size_t i = 1; i < kTapeRecordMakeupDriveKnots.size(); ++i)
    {
        const float hiKnot = kTapeRecordMakeupDriveKnots[i];
        if (clampedDrive > hiKnot)
            continue;

        const float loKnot = kTapeRecordMakeupDriveKnots[i - 1];
        const float span = hiKnot - loKnot;
        const float t = span > 0.0f ? (clampedDrive - loKnot) / span : 0.0f;
        return row[i - 1] + (row[i] - row[i - 1]) * t;
    }

    return row.back();
}

// Everything updateParameters() needs, in the domains the existing
// EchoesModel::UpdateParameters call expects.
struct Resolved
{
    float drive;
    float saturation;
    float bias;
    float wfDepthParam;      // APVTS 0..2 domain (combined wow/flutter depth)
    float wowRateScale;      // 0.5-2.0
    float flutterRateScale;  // 0.5-2.0
    bool noiseOn;
    float hissLevel;
    float condition;
    float asperityOverride;
    float dropoutOverride;
    float scrapeScale;  // 0.8 + 0.4*scrapeFlutter
    float lpfScale;
};

inline Resolved resolve(int flavorIndex, float driveKnob01)
{
    const TapeFlavorInfo& f = get(flavorIndex);
    const float d = sanitizeDriveKnob01(driveKnob01) - kDriveAnchor;

    Resolved r;
    // Per-flavor linear DRIVE law around the anchor. The law moves saturation
    // and bias with drive so the single macro changes both onset and character.
    r.drive = std::clamp(f.drive + f.kDrive * d, 0.0f, 1.0f);
    r.saturation = std::clamp(f.saturation - f.kSaturation * d, 0.12f, 0.95f);
    r.bias = std::clamp(f.bias - f.kBias * d, 0.08f, 0.90f);

    // Combined depth maps straight into the WowFlutterDepth 0-2 domain with
    // no extra scaling: the default state uses the APVTS default 0.22, which
    // equals WARM's combined 0.25*0.6+0.18*0.4 = 0.222, so WARM at its drive
    // anchor reproduces the default sound.
    const float combinedDepth = f.wowDepth * 0.6f + f.flutterDepth * 0.4f;
    r.wfDepthParam = std::clamp(combinedDepth, 0.0f, 2.0f);
    r.wowRateScale = f.wowRateScale;
    r.flutterRateScale = f.flutterRateScale;

    r.noiseOn = f.noiseEnabled;
    r.hissLevel = f.hissLevel;
    r.condition = f.condition;
    r.asperityOverride = f.asperityLevel;
    r.dropoutOverride = f.dropoutProbability;
    r.scrapeScale = 0.8f + 0.4f * f.scrapeFlutter;
    r.lpfScale = f.lpfCutoffScale;
    return r;
}

}  // namespace TapeFlavors
