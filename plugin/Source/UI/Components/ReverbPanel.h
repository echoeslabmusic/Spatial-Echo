// ReverbPanel: Reverb controls panel with size/decay/tone/level knobs and routing button.

#pragma once
#include <JuceHeader.h>
#include "ControlBlocks.h"

/**
 * ReverbPanel - Reverb controls
 *
 * Contains Size, Decay, Tone and Reverb Level knob blocks and a click-through
 * Routing button. The remaining reverb macros live in the REVERB drawer
 * (ReverbDropdown).
 *
 * Safety: Components declared BEFORE attachments to ensure correct destruction order.
 */
class ReverbPanel : public juce::Component, private juce::Timer
{
public:
    ReverbPanel(juce::AudioProcessorValueTreeState& apvts);
    ~ReverbPanel() override;

    void resized() override;

    juce::Rectangle<int> getWalkthroughOverviewScreenBounds() const;
    juce::Rectangle<int> getWalkthroughSizeDecayScreenBounds() const;
    juce::Rectangle<int> getWalkthroughToneScreenBounds() const;
    juce::Rectangle<int> getWalkthroughLevelScreenBounds() const;

    juce::Rectangle<int> getWalkthroughRoutingScreenBounds() const { return routingButton.getScreenBounds(); }

private:
    void timerCallback() override;
    void cycleRouting();
    void syncRoutingDisplay();
    void syncEngineLabels();

    juce::AudioProcessorValueTreeState& valueTreeState;
    std::atomic<float>* routingValue = nullptr;
    std::atomic<float>* engineValue = nullptr;
    int lastEngineIndex = -1;  // -1=uninitialized, 0=Spring, 1=Cloud

    // Components declared BEFORE attachments (destroyed AFTER attachments)
    KnobBlock sizeBlock;
    KnobBlock decayBlock;
    KnobBlock toneBlock;
    KnobBlock reverbLevelBlock;
    juce::TextButton routingButton;

    // Attachments declared LAST (destroyed FIRST, while components still exist)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sizeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> decayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> toneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbLevelAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReverbPanel)
};
