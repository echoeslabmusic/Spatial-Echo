// EchoEngineDropdown: Clickable "ECHO ENGINE" header: head ratio drawer plus the engine status light.

#pragma once
#include <JuceHeader.h>
#include "ParameterValueComboBox.h"
#include "SectionDropdown.h"

class SpatialEchoAudioProcessor;

/**
 * EchoEngineDropdown - the "ECHO ENGINE" section header. Clicking it opens a
 * drawer with the Head Ratio preset selector (Classic / Idealized / Golden /
 * Linear). DUB lives on the delay surface and AUTO-PANIC in the wordmark menu.
 *
 * The header label doubles as the engine status light: it tints
 * green -> amber -> red with self-oscillation heat, pulses red while the host
 * Power parameter is off, and flashes when the auto-panic watchdog fires -
 * visible even with the drawer closed.
 */
class EchoEngineDropdown : public SectionDropdown
{
public:
    enum class WalkthroughRegion
    {
        drawer,
        headRatio
    };

    EchoEngineDropdown(SpatialEchoAudioProcessor& processor, juce::AudioProcessorValueTreeState& apvts);
    ~EchoEngineDropdown() override;

    juce::Rectangle<int> getWalkthroughScreenBounds() const;
    juce::Rectangle<int> getWalkthroughScreenBounds(WalkthroughRegion region) const;

private:
    void createDropdownWindow() override;
    void pinnedStateChanged(bool pinned) override;
    void timerTick() override;  // header status light

    SpatialEchoAudioProcessor& audioProcessor;
    juce::AudioProcessorValueTreeState& valueTreeState;
    std::atomic<float>* powerParam = nullptr;  // host Power state for the header light

    // Header status light state (GUI thread only)
    int statusTick = 0;
    int autoPanicFlashTicks = 0;
    juce::uint32 lastAutoPanicEvents = 0;
    juce::Colour lastHeaderColour;

    class DropdownContent;
    juce::Component::SafePointer<DropdownContent> contentPtr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoEngineDropdown)
};

/**
 * DropdownContent - the drawer: captioned head-ratio selector with step arrows.
 */
class EchoEngineDropdown::DropdownContent : public juce::Component
{
public:
    explicit DropdownContent(juce::AudioProcessorValueTreeState& apvts);
    ~DropdownContent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    int getPreferredWidth() const;
    int getPreferredHeight() const;
    void setPinnedState(bool pinned);
    juce::Rectangle<int> getWalkthroughScreenBounds(WalkthroughRegion region) const;

    std::function<void()> onPinToggle;

private:
    juce::AudioProcessorValueTreeState& valueTreeState;

    // Preset selector with click-through arrows
    ParameterValueComboBox presetCombo;
    juce::TextButton prevButton, nextButton;
    juce::ToggleButton pinToggleButton;
    // Section caption strip, laid out in resized() and drawn in paint().
    juce::Rectangle<int> headRatioCaptionBounds;

    void stepPreset(int delta);

    // Attachments (declared last for correct destruction order)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> presetAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DropdownContent)
};
