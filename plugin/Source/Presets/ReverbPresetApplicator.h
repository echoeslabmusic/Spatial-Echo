// ReverbPresetApplicator: Apply spring and cloud reverb preset macro values to the APVTS.

#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "ReverbPresetLibrary.h"
#include "../ParameterWriteHelpers.h"

namespace SpatialEchoReverbPresets
{
inline bool applySpringPresetToState(juce::AudioProcessorValueTreeState& valueTreeState, int presetIndex,
                                     bool withGesture = true)
{
    if (presetIndex < 0 || presetIndex >= static_cast<int>(kNumReverbPresets))
        return false;

    const auto& preset = kReverbPresets[static_cast<size_t>(presetIndex)];
    bool wroteAll = true;

    auto setFloat = [&](const char* id, float value)
    { wroteAll = SpatialEchoParameterWrite::setFloatById(valueTreeState, id, value, withGesture) && wroteAll; };

    // Preset values are UI-space macros; ReverbProcessor applies its size/decay gamma tapers internally.
    setFloat("ReverbSize", preset.size);
    setFloat("ReverbDecay", preset.decay);
    setFloat("ReverbTone", preset.tone);
    setFloat("ReverbMod", preset.mod);
    setFloat("ReverbCharacter", preset.character);
    setFloat("ReverbPreDelay", preset.preDelay);
    setFloat("ReverbWidth", preset.width);

    return wroteAll;
}

inline bool applyCloudPresetToState(juce::AudioProcessorValueTreeState& valueTreeState, int presetIndex,
                                    bool withGesture = true)
{
    if (presetIndex < 0 || presetIndex >= static_cast<int>(kNumCloudReverbPresets))
        return false;

    const auto& preset = kCloudReverbPresets[static_cast<size_t>(presetIndex)];
    bool wroteAll = true;

    auto setFloat = [&](const char* id, float value)
    { wroteAll = SpatialEchoParameterWrite::setFloatById(valueTreeState, id, value, withGesture) && wroteAll; };

    // Preset values are UI-space macros; ReverbProcessor applies its size/decay gamma tapers internally.
    setFloat("ReverbSize", preset.size);
    setFloat("ReverbDecay", preset.decay);
    setFloat("ReverbTone", preset.tone);
    setFloat("ReverbMod", preset.mod);
    setFloat("ReverbCharacter", preset.character);
    setFloat("ReverbPreDelay", preset.preDelay);
    setFloat("ReverbWidth", preset.width);

    return wroteAll;
}
}  // namespace SpatialEchoReverbPresets
