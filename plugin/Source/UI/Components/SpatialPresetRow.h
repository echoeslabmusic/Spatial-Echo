// SpatialPresetRow: Compact spatial-preset stepper in the spatial panel header: prev / name / next plus REVERT.

#pragma once
#include <JuceHeader.h>
#include "CompactSelectorLookAndFeel.h"

/**
 * SpatialPresetRow - steps through SpatialPresetCatalog's 28 definitions.
 *
 * The name label always reflects live parameter state (CUSTOM when the
 * current snapshot matches no preset); REVERT re-applies the last preset the
 * user chose and lights up only while the state deviates from it.
 */
class SpatialPresetRow : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit SpatialPresetRow(juce::AudioProcessorValueTreeState& apvts);
    ~SpatialPresetRow() override;

    void resized() override;
    void mouseUp(const juce::MouseEvent& event) override;

    juce::Rectangle<int> getWalkthroughScreenBounds() const { return getScreenBounds(); }
    void showWalkthroughMenu();

    // Fired when the library popup opens (true) and closes (false). The panel
    // uses it to detach the head view's GL context while the menu is up -
    // window-parented popups are software-painted and invisible above GL.
    std::function<void(bool)> onLibraryMenuVisibility;

private:
    void timerCallback() override;  // 10 Hz: name + revert enablement sync
    void stepPreset(int delta);
    void applyPreset(int index);
    void showPresetMenu();  // flat spatial-preset popup

    juce::Rectangle<int> nameClickBounds;

    juce::AudioProcessorValueTreeState& valueTreeState;
    CompactSelectorLookAndFeel selectorLookAndFeel;

    juce::TextButton prevButton{"<"};
    juce::TextButton nextButton{">"};
    juce::Label nameLabel;
    juce::TextButton revertButton{"REVERT"};

    int lastAppliedIndex = -1;  // last preset the user explicitly selected
    int displayedIndex = -2;    // cached to avoid label churn (-2 = force refresh)
    bool displayedDeviation = false;
    bool libraryMenuVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialPresetRow)
};
