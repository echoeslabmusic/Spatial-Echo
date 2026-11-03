// SpatialMotionSection: Inline motion control surface: motion cards + choreography selector + RATE and DEPTH macro rows
//   + FINE-TUNE drawer.

#pragma once
#include <JuceHeader.h>
#include "MotionCardStrip.h"
#include "CompactSelectorLookAndFeel.h"
#include "DivisionMenuGrid.h"
#include "DropdownWindowUtils.h"
#include "ParameterValueComboBox.h"
#include "ParameterValueSlider.h"

/**
 * SpatialMotionSection - everything motion, permanently visible under the
 * head view. Nothing here ever covers the 3D scene.
 *
 * Rows (top to bottom):
 *   - MotionCardStrip (named patterns with animated previews)
 *   - RATE: rate slider (free) or note-division picker (synced) + SYNC toggle
 *   - DEPTH: motion macro slider + live effective-swing readout
 *   - FINE-TUNE drawer: choreography (UNISON/CONSTELLATION/CASCADE + SPREAD),
 *     and retrigger mode - the advanced motion controls, one click away. The
 *     FINE-TUNE button shows a deviation dot while the drawer hides non-default
 *     state the cards cannot explain.
 *
 * The drawer changes the section's preferred height; the owning panel
 * re-lays out via onLayoutChanged.
 */
class SpatialMotionSection : public juce::Component, private juce::Timer
{
public:
    explicit SpatialMotionSection(juce::AudioProcessorValueTreeState& apvts);
    ~SpatialMotionSection() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    int getPreferredHeight() const;
    bool isFineTuneVisible() const { return fineTuneVisible; }
    void setFineTuneVisibleForWalkthrough(bool visible);

    juce::Rectangle<int> getWalkthroughPatternScreenBounds() const;
    juce::Rectangle<int> getWalkthroughRateScreenBounds() const;
    juce::Rectangle<int> getWalkthroughSyncScreenBounds() const;
    juce::Rectangle<int> getWalkthroughDivisionControlScreenBounds() const;
    juce::Rectangle<int> getWalkthroughDepthScreenBounds() const;
    juce::Rectangle<int> getWalkthroughFineTuneButtonScreenBounds() const;
    juce::Rectangle<int> getWalkthroughChoreographyScreenBounds() const;
    juce::Rectangle<int> getWalkthroughSpreadScreenBounds() const;
    juce::Rectangle<int> getWalkthroughRetriggerScreenBounds() const;
    juce::Rectangle<int> getWalkthroughDivisionMenuScreenBounds() const;

    bool isWalkthroughDivisionMenuVisible() const;
    void showWalkthroughDivisionMenu();
    void dismissWalkthroughDivisionMenu();
    void setWalkthroughNavigationComponent(juce::Component* component);
    std::function<void()> onLayoutChanged;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    void setFineTuneVisibleForTesting(bool visible)
    {
        setFineTuneVisibleForWalkthrough(visible);
        resized();
    }

    bool hasValidDepthReadoutLayoutForTesting() const
    {
        return ParameterValueLayout::fits(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize),
                                          depthReadout.getText(), depthReadout.getLocalBounds());
    }

    float getDepthReadoutFontHeightForTesting() const { return depthReadout.getFont().getHeight(); }

    void selectDivisionFromClosedComboForTesting(int menuIndex)
    {
        divisionCombo.setSelectedId(menuIndex + 1, juce::sendNotificationSync);
    }
#endif

private:
    class DivisionComboBox final : public ParameterValueComboBox
    {
    public:
        std::function<void()> onPopupRequested;

        void showPopup() override
        {
            if (!isEnabled())
                return;

            if (onPopupRequested)
                onPopupRequested();
            else
                ParameterValueComboBox::showPopup();
        }
    };

    void timerCallback() override;  // 10 Hz: depth readout refresh
    void updateSyncVisibility(bool synced);
    int getCombinedDivisionIndex() const;
    void updateDivisionSelection();
    void setCombinedDivisionIndex(int index);
    void showDivisionMenu();
    void dismissDivisionMenu(bool restoreFocus = true);
    void updateSpreadEnablement();
    void setFineTuneVisible(bool visible);
    void styleRowLabel(juce::Label& label, const juce::String& text);
    void setupCompactSlider(ParameterValueSlider& slider, const juce::String& tooltip);
    void setupCompactCombo(ParameterValueComboBox& combo, const char* paramId, const juce::String& tooltip);

    juce::AudioProcessorValueTreeState& valueTreeState;
    CompactSelectorLookAndFeel selectorLookAndFeel;

    MotionCardStrip cardStrip{valueTreeState};

    // Choreography row
    juce::Label choreographyLabel;
    juce::TextButton unisonButton{"UNISON"};
    juce::TextButton constellationButton{"CONSTELLATION"};
    juce::TextButton cascadeButton{"CASCADE"};
    juce::Label spreadLabel;
    ParameterValueSlider spreadSlider;

    // RATE row
    juce::Label rateLabel;
    ParameterValueSlider rateSlider;
    DivisionComboBox divisionCombo;
    juce::TextButton syncButton{"SYNC"};

    // DEPTH row
    juce::Label depthLabel;
    ParameterValueSlider depthSlider;
    juce::Label depthReadout;
    juce::TextButton fineTuneButton{"FINE-TUNE"};

    // FINE-TUNE drawer (minimal: RETRIG, CHOREO, SPREAD only)
    bool fineTuneVisible = false;
    juce::Label retrigLabel;
    ParameterValueComboBox retrigCombo;

    juce::String lastDepthReadout;
    bool fineTuneDirty = false;  // drawer holds non-default hidden state

    // Parameter wiring (attachments last: destroyed before the components).
    std::unique_ptr<juce::ParameterAttachment> choreoAttachment;
    std::unique_ptr<juce::ParameterAttachment> syncVisibilityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> spreadAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> rateAttachment;
    std::unique_ptr<juce::ParameterAttachment> divisionAttachment;
    std::unique_ptr<juce::ParameterAttachment> longDivisionAttachment;
    std::unique_ptr<juce::ParameterAttachment> longVariantDivisionAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> syncAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> depthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> retrigAttachment;
    std::unique_ptr<SpatialEchoDropdownWindow::Host> divisionMenuWindow;
    juce::Component::SafePointer<juce::Component> walkthroughNavigationComponent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialMotionSection)
};
