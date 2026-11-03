// SpatialEchoParameterWrite: Utilities for APVTS parameter writes with diagnostic fallbacks (begin/end gestures,
//   setNormalized).

#pragma once

#include <JuceHeader.h>

namespace SpatialEchoParameterWrite
{
namespace detail
{
// Diagnostic for a parameter write that could not be completed. The setter
// still returns false (callers decide how to react), but the reason and the
// offending parameter id are surfaced in debug builds so mis-wired ids do
// not fail silently. Compiles to nothing in release.
inline bool reportWriteFailure(juce::StringRef parameterId, const char* reason)
{
    DBG(juce::String("SpatialEchoParameterWrite: ") + reason + " for parameter '" + juce::String(parameterId) + "'");
    juce::ignoreUnused(parameterId, reason);
    return false;
}
}  // namespace detail

inline bool beginGestureById(juce::AudioProcessorValueTreeState& valueTreeState, juce::StringRef parameterId)
{
    if (auto* parameter = valueTreeState.getParameter(parameterId))
    {
        parameter->beginChangeGesture();
        return true;
    }

    return detail::reportWriteFailure(parameterId, "parameter not found");
}

inline bool endGestureById(juce::AudioProcessorValueTreeState& valueTreeState, juce::StringRef parameterId)
{
    if (auto* parameter = valueTreeState.getParameter(parameterId))
    {
        parameter->endChangeGesture();
        return true;
    }

    return detail::reportWriteFailure(parameterId, "parameter not found");
}

inline bool setNormalized(juce::RangedAudioParameter* parameter, float normalizedValue, bool withGesture = false)
{
    if (parameter == nullptr)
        return false;

    const float clamped = juce::jlimit(0.0f, 1.0f, normalizedValue);
    if (withGesture)
        parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(clamped);
    if (withGesture)
        parameter->endChangeGesture();
    return true;
}

inline bool setFloatById(juce::AudioProcessorValueTreeState& valueTreeState, juce::StringRef parameterId,
                         float plainValue, bool withGesture = false)
{
    if (auto* parameter = dynamic_cast<juce::AudioParameterFloat*>(valueTreeState.getParameter(parameterId)))
        return setNormalized(parameter, parameter->convertTo0to1(plainValue), withGesture);

    return detail::reportWriteFailure(parameterId, "float parameter not found or wrong type");
}

inline bool setBoolById(juce::AudioProcessorValueTreeState& valueTreeState, juce::StringRef parameterId, bool value,
                        bool withGesture = false)
{
    if (auto* parameter = dynamic_cast<juce::AudioParameterBool*>(valueTreeState.getParameter(parameterId)))
        return setNormalized(parameter, value ? 1.0f : 0.0f, withGesture);

    return detail::reportWriteFailure(parameterId, "bool parameter not found or wrong type");
}

inline bool setChoiceById(juce::AudioProcessorValueTreeState& valueTreeState, juce::StringRef parameterId, int index,
                          bool withGesture = false)
{
    if (auto* parameter = dynamic_cast<juce::AudioParameterChoice*>(valueTreeState.getParameter(parameterId)))
    {
        const int maxIndex = juce::jmax(0, parameter->choices.size() - 1);
        const int clamped = juce::jlimit(0, maxIndex, index);
        const float normalized = (maxIndex > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIndex) : 0.0f;
        return setNormalized(parameter, normalized, withGesture);
    }

    return detail::reportWriteFailure(parameterId, "choice parameter not found or wrong type");
}
}  // namespace SpatialEchoParameterWrite
