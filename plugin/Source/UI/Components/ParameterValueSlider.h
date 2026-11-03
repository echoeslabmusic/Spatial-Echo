// ParameterValueSlider: APVTS-aware Slider value sizing with no-shrink fallback, plus Shift-drag fine adjustment.
// Realtime-safety: message-thread UI use only

#pragma once

#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "ParameterValueLayout.h"
#include "../UIConstants.h"
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>

class ParameterValueSlider : public juce::Slider
{
public:
    using ToText = std::function<juce::String(double)>;
    using FromText = std::function<double(const juce::String&)>;

    ParameterValueSlider() = default;

    void setTextBoxStyle(TextEntryBoxPosition position, bool readOnly, int minimumWidth, int height)
    {
        textBoxPosition = position;
        textBoxReadOnly = readOnly;
        textBoxHeight = height;
        minimumTextBoxWidth = juce::jmax(0, minimumWidth);
        resolvedTextBoxWidth = juce::jmax(resolvedTextBoxWidth, minimumTextBoxWidth);

        juce::Slider::setTextBoxStyle(textBoxPosition, textBoxReadOnly, resolvedTextBoxWidth, textBoxHeight);

        envelopeSignature.clear();
        if (textBoxPosition != NoTextBox)
            refreshValueTextEnvelope();
    }

    void setValueTextFormatter(ToText toText, FromText fromText = {})
    {
        textFromValueFunction = std::move(toText);
        if (fromText)
            valueFromTextFunction = std::move(fromText);

        envelopeSignature.clear();
        refreshValueTextEnvelope();
        updateText();
    }

    void refreshValueTextEnvelope()
    {
        if (textBoxPosition == NoTextBox || refreshingEnvelope || getLocalBounds().isEmpty())
            return;

        const juce::ScopedValueSetter<bool> guard(refreshingEnvelope, true);
        const auto font = valueFont();
        int required = minimumTextBoxWidth;
        std::unordered_set<juce::String> uniqueTexts;

        const double minimum = getMinimum();
        const double maximum = getMaximum();
        const double interval = getInterval();
        const double span = maximum - minimum;
        const auto stepCount =
            interval > 0.0 && span >= 0.0 ? static_cast<long long>(std::floor(span / interval + 0.5)) + 1 : 0LL;

        if (stepCount > 0 && stepCount <= ParameterValueLayout::maximumEnumeratedSteps)
        {
            for (long long i = 0; i < stepCount; ++i)
            {
                const double value = i + 1 == stepCount ? maximum : minimum + static_cast<double>(i) * interval;
                uniqueTexts.insert(getTextFromValue(value));
            }
        }
        else
        {
            for (int i = 0; i < ParameterValueLayout::continuousSampleCount; ++i)
            {
                const double normalized =
                    static_cast<double>(i) / static_cast<double>(ParameterValueLayout::continuousSampleCount - 1);
                const double value = proportionOfLengthToValue(normalized);
                uniqueTexts.insert(getTextFromValue(value));
            }
        }

        for (const auto& text : uniqueTexts)
            required = juce::jmax(required, ParameterValueLayout::requiredWidth(font, text));

        applyResolvedWidth(required);
        envelopeSignature = makeEnvelopeSignature();
    }

    int getResolvedTextBoxWidth() const noexcept { return resolvedTextBoxWidth; }

    float getValueFontHeight() const { return valueFont().getHeight(); }

    int getMinimumRequiredComponentWidth() const noexcept
    {
        switch (textBoxPosition)
        {
            case TextBoxLeft:
            case TextBoxRight:
                return resolvedTextBoxWidth + ParameterValueLayout::minimumHorizontalTrackWidth;
            case TextBoxAbove:
            case TextBoxBelow:
                return resolvedTextBoxWidth;
            case NoTextBox:
                return 0;
        }
        return resolvedTextBoxWidth;
    }

    bool hasValidValueLayout() const noexcept
    {
        return textBoxPosition == NoTextBox || getWidth() >= getMinimumRequiredComponentWidth();
    }

    // Holding Shift while dragging moves the value at kFineDragScale of the
    // normal rate. The slider tracks a virtual pointer that advances by the
    // scaled motion, so pressing or releasing Shift mid-drag never jumps.
    void mouseDown(const juce::MouseEvent& e) override
    {
        lastDragPosition = virtualDragPosition = e.position;
        juce::Slider::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const float scale = e.mods.isShiftDown() ? kFineDragScale : 1.0f;
        virtualDragPosition += (e.position - lastDragPosition) * scale;
        lastDragPosition = e.position;
        juce::Slider::mouseDrag(e.withNewPosition(virtualDragPosition));
    }

protected:
    void valueChanged() override
    {
        if (!refreshingEnvelope && textBoxPosition != NoTextBox)
        {
            if (makeEnvelopeSignature() != envelopeSignature)
                refreshValueTextEnvelope();

            applyResolvedWidth(ParameterValueLayout::requiredWidth(valueFont(), getTextFromValue(getValue())));
        }

        juce::Slider::valueChanged();
    }

    void resized() override
    {
        juce::Slider::resized();
        if (!getLocalBounds().isEmpty())
        {
            if (!refreshingEnvelope && textBoxPosition != NoTextBox && makeEnvelopeSignature() != envelopeSignature)
                refreshValueTextEnvelope();

            jassert(hasValidValueLayout());
        }
    }

private:
    static constexpr float kFineDragScale = 0.1f;

    static juce::Font valueFont() { return CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize); }

    juce::String makeEnvelopeSignature()
    {
        const double minimum = getMinimum();
        const double maximum = getMaximum();
        const double midpoint = proportionOfLengthToValue(0.5);
        juce::String signature;
        signature << juce::String(minimum, 15) << '|' << juce::String(maximum, 15) << '|'
                  << juce::String(getInterval(), 15) << '|' << getTextFromValue(minimum) << '|'
                  << getTextFromValue(midpoint) << '|' << getTextFromValue(maximum);
        return signature;
    }

    void applyResolvedWidth(int required)
    {
        const int grown = juce::jmax(minimumTextBoxWidth, juce::jmax(resolvedTextBoxWidth, required));
        if (grown == resolvedTextBoxWidth)
            return;

        resolvedTextBoxWidth = grown;
        juce::Slider::setTextBoxStyle(textBoxPosition, textBoxReadOnly, resolvedTextBoxWidth, textBoxHeight);
    }

    TextEntryBoxPosition textBoxPosition = TextBoxLeft;
    bool textBoxReadOnly = false;
    int textBoxHeight = 20;
    int minimumTextBoxWidth = 0;
    int resolvedTextBoxWidth = 0;
    bool refreshingEnvelope = false;
    juce::String envelopeSignature;
    juce::Point<float> lastDragPosition, virtualDragPosition;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterValueSlider)
};
