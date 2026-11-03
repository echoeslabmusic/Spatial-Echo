// ParameterValueLayout: Shared parameter-value measurement and fit invariants.
// Realtime-safety: message-thread UI use only

#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <utility>

namespace ParameterValueLayout
{
constexpr int totalHorizontalPadding = 12;
constexpr int minimumHorizontalTrackWidth = 24;
constexpr int continuousSampleCount = 1025;
constexpr int maximumEnumeratedSteps = 4096;
constexpr int comboArrowReserve = 20;

inline int requiredWidth(const juce::Font& font, const juce::String& text, int padding = totalHorizontalPadding)
{
    return juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(font, text))) + padding;
}

template <typename Formatter>
int requiredEnvelopeWidth(const juce::Font& font, int discreteSteps, Formatter&& formatter)
{
    const int samples =
        discreteSteps > 0 && discreteSteps <= maximumEnumeratedSteps ? discreteSteps : continuousSampleCount;

    int width = 0;
    for (int i = 0; i < samples; ++i)
    {
        const double normalized = samples == 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(samples - 1);
        width = juce::jmax(width, requiredWidth(font, formatter(normalized)));
    }
    return width;
}

inline int requiredParameterEnvelopeWidth(const juce::Font& font, const juce::RangedAudioParameter& parameter)
{
    const int steps = parameter.isDiscrete() ? parameter.getNumSteps() : 0;
    return requiredEnvelopeWidth(font, steps, [&parameter](double normalized)
                                 { return parameter.getText(static_cast<float>(normalized), 1024); });
}

inline bool fits(const juce::Font& font, const juce::String& text, juce::Rectangle<int> bounds,
                 int padding = totalHorizontalPadding)
{
    return bounds.getWidth() >= requiredWidth(font, text, padding);
}

inline void assertFits(const juce::Component& owner, const juce::Font& font, const juce::String& text,
                       juce::Rectangle<int> bounds)
{
    juce::ignoreUnused(owner, font, text, bounds);
    jassert(fits(font, text, bounds));
}

inline void drawValueText(juce::Graphics& graphics, const juce::Component& owner, const juce::Font& font,
                          const juce::String& text, juce::Rectangle<int> bounds, juce::Justification justification)
{
    assertFits(owner, font, text, bounds);
    graphics.setFont(font);
    graphics.drawText(text, bounds, justification, false);
}
}  // namespace ParameterValueLayout
