// ParameterValueComboBox: Finite-choice value measurement and parent-width contract.
// Realtime-safety: message-thread UI use only

#pragma once

#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "ParameterValueLayout.h"
#include "../UIConstants.h"

class ParameterValueComboBox : public juce::ComboBox
{
public:
    ParameterValueComboBox() = default;

    int getRequiredValueWidth() const
    {
        const auto font = CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize);
        int required = ParameterValueLayout::requiredWidth(font, getText());
        for (int i = 0; i < getNumItems(); ++i)
            required = juce::jmax(required, ParameterValueLayout::requiredWidth(font, getItemText(i)));
        return required + ParameterValueLayout::comboArrowReserve;
    }

    bool hasValidValueLayout() const { return getWidth() >= getRequiredValueWidth(); }

    float getValueFontHeight() const
    {
        return CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize).getHeight();
    }

    void resized() override
    {
        juce::ComboBox::resized();
        if (!getLocalBounds().isEmpty())
            jassert(hasValidValueLayout());
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterValueComboBox)
};
