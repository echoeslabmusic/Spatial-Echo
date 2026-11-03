// SpatialPositionEditor: Exact-entry popover behind the head view's corner readout: az/el/dist typed entry + per-axis
//   locks.

#pragma once
#include <JuceHeader.h>

/**
 * SpatialPositionEditor - shown inside a CallOutBox when the head view's
 * corner az/el/dist readout is clicked.
 *
 * Three rows (AZIMUTH / ELEV / DIST): label + editable value + padlock.
 * Locks attach directly to the SpatialLock* parameters (they persist after
 * the popover closes); typed values route back through onAxisEdit so the
 * target-aware application logic stays in SpatialPanel. Out-of-range entries
 * are clamped by the receiver but flagged here with an amber flash.
 */
class SpatialPositionEditor : public juce::Component, private juce::Timer
{
public:
    explicit SpatialPositionEditor(juce::AudioProcessorValueTreeState& apvts);
    ~SpatialPositionEditor() override;

    // Pull the selected target's plain value for an axis (0=az, 1=el, 2=dist).
    std::function<float(int axis)> getAxisValue;
    // Apply a typed plain value to an axis (clamping happens in the receiver).
    std::function<void(int axis, float value)> onAxisEdit;

    void resized() override;
    void paint(juce::Graphics& g) override;

    static constexpr int preferredWidth = 212;
    static constexpr int preferredHeight = 3 * 24 + 2 * 4 + 2 * 8;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    bool hasValidValueLayoutForTesting() const;
    float getValueFontHeightForTesting() const;
#endif

private:
    struct LockLookAndFeel : public juce::LookAndFeel_V4
    {
        void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    };

    void timerCallback() override;  // 10 Hz: mirror external changes
    void refreshValues(bool force);
    void handleEdit(int axis);
    void flashRangeError(int axis);

    juce::AudioProcessorValueTreeState& valueTreeState;
    LockLookAndFeel lockLookAndFeel;

    std::array<juce::Label, 3> axisLabels;
    std::array<juce::Label, 3> axisValues;
    std::array<juce::ToggleButton, 3> axisLocks;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 3> lockAttachments;

    int errorFlashAxis = -1;
    bool suppressCallbacks = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialPositionEditor)
};
