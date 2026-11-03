// CompactSelectorLookAndFeel: Shared compact selector button style (thin bracket when off, filled bracket when on) used
//   by the spatial panel's mode/target buttons and the motion section's selectors.

#pragma once
#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "../UIConstants.h"

struct CompactSelectorLookAndFeel : public juce::LookAndFeel_V4
{
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(backgroundColour);
        const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        const bool isOn = button.getToggleState();
        const auto green = CRTLookAndFeel::getTerminalGreen();
        const bool isEnabled = button.isEnabled();

        if (isOn)
        {
            float fillAlpha = shouldDrawButtonAsDown ? 0.38f : 0.28f;
            fillAlpha *= isEnabled ? 1.0f : 0.4f;
            g.setColour(green.withAlpha(fillAlpha));
            g.fillRoundedRectangle(bounds, 2.0f);
            float borderAlpha = 0.95f * (isEnabled ? 1.0f : 0.4f);
            g.setColour(green.withAlpha(borderAlpha));
            g.drawRoundedRectangle(bounds, 2.0f, 1.2f);
        }
        else
        {
            if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
            {
                float fillAlpha = 0.08f * (isEnabled ? 1.0f : 0.4f);
                g.setColour(green.withAlpha(fillAlpha));
                g.fillRoundedRectangle(bounds, 2.0f);
            }
            float borderAlpha = (shouldDrawButtonAsHighlighted ? 0.55f : 0.35f) * (isEnabled ? 1.0f : 0.4f);
            g.setColour(green.withAlpha(borderAlpha));
            g.drawRoundedRectangle(bounds, 2.0f, 1.0f);
        }
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused(shouldDrawButtonAsDown);
        const bool isOn = button.getToggleState();
        const auto green = CRTLookAndFeel::getTerminalGreen();
        const bool isEnabled = button.isEnabled();
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
        float textAlpha = (isOn ? 1.0f : (shouldDrawButtonAsHighlighted ? 0.85f : 0.62f)) * (isEnabled ? 1.0f : 0.4f);
        g.setColour(green.withAlpha(textAlpha));
        g.drawText(button.getButtonText().toUpperCase(), button.getLocalBounds(), juce::Justification::centred);
    }
};
