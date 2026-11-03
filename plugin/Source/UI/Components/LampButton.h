// LampButton: Latching toggle chip that turns amber, pulses, and spells out its engaged state so a muting mode never
//   looks broken.

#pragma once
#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "ParameterValueLayout.h"

/**
 * LampButton - A latching toggle chip for modes that silence something.
 * Off: a small, dim green chip reading offText ("DUB") - a minor switch.
 * On: it widens into an amber chip reading onText ("DUB: INPUT OFF") that
 * pulses slowly, so a state recalled from a session or automation is as
 * visible as a fresh click. The component reserves the engaged width so
 * neighbours never move; only the drawn chip takes clicks.
 *
 * Usage: attach to a boolean parameter via custom onClick callback with inverted logic.
 */
class LampButton : public juce::Button, private juce::Value::Listener, private juce::Timer
{
public:
    LampButton(const juce::String& offTextToShow, const juce::String& onTextToShow)
        : juce::Button(offTextToShow)
        , offText(offTextToShow)
        , onText(onTextToShow)
    {
        setClickingTogglesState(true);
        setWantsKeyboardFocus(true);
        getToggleStateValue().addListener(this);
    }

    ~LampButton() override { getToggleStateValue().removeListener(this); }

    // Width that fits the longer of the two labels (no-truncation rule).
    int getRequiredWidth() const { return juce::jmax(chipWidthFor(false), chipWidthFor(true)); }

    // Horizontal centre (local x) the chip aims for; it stays inside the bounds.
    void setChipCentreX(int localX)
    {
        chipCentreX = localX;
        repaint();
    }

    bool hitTest(int x, int y) override
    {
        juce::ignoreUnused(y);
        const auto chip = getChipBounds();
        return x >= chip.getX() && x < chip.getRight();
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const bool lit = getToggleState();
        auto bounds = getChipBounds().toFloat().reduced(0.5f);

        if (lit)
        {
            // Slow amber breathing: ~2 s cycle between 0 and 1.
            const float pulse = 0.5f + 0.5f * std::sin(pulsePhase);
            const auto amber = CRTLookAndFeel::getWarningAmber();
            float fillAlpha = 0.12f + 0.14f * pulse;
            if (shouldDrawButtonAsDown)
                fillAlpha += 0.08f;
            else if (shouldDrawButtonAsHighlighted)
                fillAlpha += 0.04f;
            g.setColour(amber.withAlpha(fillAlpha));
            g.fillRoundedRectangle(bounds, 3.0f);
            g.setColour(amber.withAlpha(0.70f + 0.30f * pulse));
            g.drawRoundedRectangle(bounds, 3.0f, 1.5f);
            g.setColour(amber);
        }
        else
        {
            const auto green = CRTLookAndFeel::getTerminalGreen();
            if (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted)
            {
                g.setColour(green.withAlpha(shouldDrawButtonAsDown ? 0.14f : 0.08f));
                g.fillRoundedRectangle(bounds, 3.0f);
            }
            g.setColour(green.withAlpha(0.35f));
            g.drawRoundedRectangle(bounds, 3.0f, 1.0f);
            g.setColour(green.withAlpha(0.55f));
        }

        g.setFont(getLabelFont(lit));
        g.drawText(lit ? onText : offText, getChipBounds(), juce::Justification::centred, false);
    }

private:
    static constexpr int kTextInset = 6;
    static constexpr int kChipHeight = 18;

    // Off reads as a minor switch; on uses the toggle text size so it is legible.
    static juce::Font getLabelFont(bool lit)
    {
        return CRTLookAndFeel::getTerminalFontStatic(lit ? UIConstants::typeLabelSize : UIConstants::typeMicroSize);
    }

    int chipWidthFor(bool lit) const
    {
        return ParameterValueLayout::requiredWidth(getLabelFont(lit), lit ? onText : offText) + 2 * kTextInset;
    }

    juce::Rectangle<int> getChipBounds() const
    {
        const int w = juce::jmin(getWidth(), chipWidthFor(getToggleState()));
        const int centre = chipCentreX.value_or(getWidth() / 2);
        return getLocalBounds()
            .withSizeKeepingCentre(w, juce::jmin(getHeight(), kChipHeight))
            .withX(juce::jlimit(0, getWidth() - w, centre - w / 2));
    }

    void valueChanged(juce::Value&) override
    {
        if (getToggleState())
        {
            pulsePhase = 0.0f;
            startTimerHz(30);
        }
        else
        {
            stopTimer();
        }
        repaint();
    }

    void timerCallback() override
    {
        pulsePhase += juce::MathConstants<float>::twoPi / 60.0f;
        if (pulsePhase >= juce::MathConstants<float>::twoPi)
            pulsePhase -= juce::MathConstants<float>::twoPi;
        repaint();
    }

    juce::String offText;
    juce::String onText;
    float pulsePhase = 0.0f;
    std::optional<int> chipCentreX;
};
