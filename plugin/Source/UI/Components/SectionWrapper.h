// SectionWrapper: Decorative CRT frame drawn behind one of the editor's two main sections.

#pragma once

#include <JuceHeader.h>
#include "CRTLookAndFeel.h"

/**
 * SectionWrapper - the rounded phosphor frame with corner accents around the
 * ECHO ENGINE and SPATIAL sections. It sits behind the section's controls and
 * never intercepts the mouse.
 */
class SectionWrapper : public juce::Component
{
public:
    SectionWrapper() { setInterceptsMouseClicks(false, false); }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        g.setColour(juce::Colour(0, 200, 0).withAlpha(0.6f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 3.0f, 1.5f);
        CRTLookAndFeel::drawCornerAccents(g, bounds, 8.0f, 2.0f);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionWrapper)
};
