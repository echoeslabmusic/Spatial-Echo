// SpatialRoutingDrawer: Collapsible BLEND drawer for the panel sidebar: signal chain, wet mix, width, crossover, render
//   quality.

#pragma once
#include <JuceHeader.h>
#include "CRTLookAndFeel.h"
#include "ParameterValueComboBox.h"
#include "ParameterValueSlider.h"
#include "../UIConstants.h"

/**
 * SpatialRoutingDrawer - collapsible BLEND drawer (one disclosure row that
 * expands into the five signal-shaping controls). Collapsed by default;
 * expanding changes the preferred height and the owning panel re-lays out via
 * onLayoutChanged.
 */
class SpatialRoutingDrawer : public juce::Component, private juce::Timer
{
public:
    explicit SpatialRoutingDrawer(juce::AudioProcessorValueTreeState& apvts);
    ~SpatialRoutingDrawer() override { stopTimer(); }

    void resized() override;
    void mouseUp(const juce::MouseEvent& event) override;
    void paint(juce::Graphics& g) override;

    int getPreferredHeight() const;
    bool isExpanded() const { return expanded; }
    void setExpandedForWalkthrough(bool visible)
    {
        if (expanded != visible)
            setExpanded(visible);
    }

    juce::Rectangle<int> getWalkthroughQualityScreenBounds() const;
    juce::Rectangle<int> getWalkthroughChainScreenBounds() const;
    juce::Rectangle<int> getWalkthroughWetScreenBounds() const;
    juce::Rectangle<int> getWalkthroughWidthScreenBounds() const;
    juce::Rectangle<int> getWalkthroughCrossoverScreenBounds() const;
    std::function<void()> onLayoutChanged;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    void setExpandedForTesting(bool visible)
    {
        setExpandedForWalkthrough(visible);
        resized();
    }
#endif

private:
    void timerCallback() override;  // 2 Hz: deviation-dot refresh
    void styleRowLabel(juce::Label& label, const juce::String& text);
    void setExpanded(bool shouldExpand);

    juce::AudioProcessorValueTreeState& valueTreeState;
    bool expanded = false;
    bool contentsDirty = false;  // any routing param off its default
    juce::Rectangle<int> headerBounds;

    juce::Label chainLabel, wetLabel, widthLabel, crossoverLabel, qualityLabel;
    ParameterValueComboBox chainCombo, qualityCombo;
    ParameterValueSlider wetSlider, widthSlider, crossoverSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> chainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> wetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> widthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> crossoverAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> qualityAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialRoutingDrawer)
};
