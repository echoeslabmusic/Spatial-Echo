// SpatialPanel: Full-height spatial controller: header strip + 3D head view (the single focal point) + motion section +
//   drawers.

#pragma once
#include <JuceHeader.h>
#include "SpatialHeadView.h"
#include "CRTLookAndFeel.h"
#include "CompactSelectorLookAndFeel.h"
#include "SpatialMotionSection.h"
#include "SpatialPresetRow.h"
#include "SpatialRoutingDrawer.h"

/**
 * SpatialPanel - the right panel, dedicated to spatial control
 *
 * Layout (one focal point, no boxes, no left column):
 *   Header row:   [ENABLE]        [< preset > REVERT]           [PER-HEAD]
 *   Head view:    full width - drag = az/el, wheel = dist, marker click =
 *                 tap select, corner readout click = exact-entry popover
 *                 (SpatialPositionEditor with the per-axis locks). Tap chips
 *                 (H1 H2 H3 ALL) overlay top-left and lock when unavailable.
 *   Motion:       SpatialMotionSection (cards + RATE/DEPTH + FINE-TUNE).
 *   Routing:      SpatialRoutingDrawer (collapsed disclosure row).
 *
 * SpatialHeadView handles the 3D visualisation + mouse interaction.
 * Target selection and mode switching are owned here.
 *
 * Safety: Components declared BEFORE attachments to ensure correct destruction order.
 */
class SpatialPanel : public juce::Component
{
public:
    SpatialPanel(juce::AudioProcessorValueTreeState& apvts);
    ~SpatialPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Access to the head view for callback wiring from parent
    SpatialHeadView& getHeadView() { return headView; }

    // First-load hint pointing at PER-HEAD, drawn in the head view while the
    // instance stays Linked. It is dismissed the first time per-head mode
    // engages; the editor persists that per user via onPerHeadHintDismissed.
    void setPerHeadHintDismissed(bool dismissed);
    bool isPerHeadHintDismissed() const { return perHeadHintDismissed; }
    std::function<void()> onPerHeadHintDismissed;

    juce::Rectangle<int> getWalkthroughEnableScreenBounds() const;
    juce::Rectangle<int> getWalkthroughPresetScreenBounds() const;
    juce::Rectangle<int> getWalkthroughModeScreenBounds() const;
    juce::Rectangle<int> getWalkthroughHeadSelectorScreenBounds() const;
    juce::Rectangle<int> getWalkthroughHeadViewScreenBounds() const;
    juce::Rectangle<int> getWalkthroughPositionReadoutScreenBounds() const;
    juce::Rectangle<int> getWalkthroughPositionEditorScreenBounds() const;

    void showWalkthroughPresetMenu();
    bool isWalkthroughPositionEditorVisible() const;
    void setWalkthroughPositionEditorVisible(bool visible);
    void revealWalkthroughDistanceReferences();

    juce::Rectangle<int> getWalkthroughBlendQualityScreenBounds() const;
    juce::Rectangle<int> getWalkthroughBlendChainScreenBounds() const;
    juce::Rectangle<int> getWalkthroughBlendWetScreenBounds() const;
    juce::Rectangle<int> getWalkthroughBlendWidthScreenBounds() const;
    juce::Rectangle<int> getWalkthroughBlendCrossoverScreenBounds() const;

    juce::Rectangle<int> getWalkthroughMotionPatternScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionRateScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionSyncScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionDivisionControlScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionDepthScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionFineTuneButtonScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionChoreographyScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionSpreadScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionRetriggerScreenBounds() const;
    juce::Rectangle<int> getWalkthroughMotionDivisionMenuScreenBounds() const;

    bool isWalkthroughMotionDivisionMenuVisible() const;
    void showWalkthroughMotionDivisionMenu();
    void dismissWalkthroughMotionDivisionMenu();
    void setWalkthroughMotionNavigationComponent(juce::Component* component);

    juce::Rectangle<int> getWalkthroughPositionScreenBounds() const
    {
        return enableButton.getScreenBounds()
            .getUnion(perTapModeButton.getScreenBounds())
            .getUnion(headView.getScreenBounds());
    }

    juce::Rectangle<int> getWalkthroughBlendScreenBounds() const { return routingDrawer.getScreenBounds(); }

    juce::Rectangle<int> getWalkthroughMotionScreenBounds() const { return motionSection.getScreenBounds(); }

    bool isWalkthroughBlendExpanded() const { return routingDrawer.isExpanded(); }
    bool isWalkthroughMotionFineTuneVisible() const { return motionSection.isFineTuneVisible(); }
    void setWalkthroughBlendExpanded(bool visible) { routingDrawer.setExpandedForWalkthrough(visible); }
    void setWalkthroughMotionFineTuneVisible(bool visible) { motionSection.setFineTuneVisibleForWalkthrough(visible); }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    void setValueDisclosuresForTesting(bool motionFineTune, bool blendExpanded)
    {
        motionSection.setFineTuneVisibleForTesting(motionFineTune);
        routingDrawer.setExpandedForTesting(blendExpanded);
        resized();
    }
#endif

private:
    juce::AudioProcessorValueTreeState& valueTreeState;

    // Compact selector style for mode/target buttons (shared with the motion
    // section's selectors so all bracket buttons match).
    CompactSelectorLookAndFeel compactButtonLookAndFeel;

    // The exact-entry popover is desktop-parented (an editor-child callout is
    // composited under the GL head view), so it cannot inherit the editor's
    // LookAndFeel - it gets this one explicitly. The SafePointer lets the
    // destructor dismiss a still-open box before the LnF dies.
    CRTLookAndFeel calloutLookAndFeel;
    juce::Component::SafePointer<juce::CallOutBox> positionEditorBox;

    // Numeric edit routed to the current target (0=az, 1=el, 2=dist).
    // Called by the exact-entry popover; clamps out-of-range values.
    void applyNumericEdit(int axis, float value);
    // Reflect the current edit target / mode into the chips + PER-HEAD toggle.
    void syncTargetButtons();
    // Exact-entry popover anchored to the head view's corner readout.
    void showPositionEditor();
    bool isLockParamSet(const char* paramId) const;

    // Components declared BEFORE attachments (destroyed AFTER attachments)
    juce::ToggleButton enableButton;
    SpatialHeadView headView;

    // Mode toggle - writes the HeadPositionMode parameter (off = Linked).
    juce::TextButton perTapModeButton{"PER-HEAD"};

    // Tap chips overlaying the head view. Echo Mode determines which per-head
    // targets are currently audible/editable.
    juce::TextButton h1TargetButton{"H1"};
    juce::TextButton h2TargetButton{"H2"};
    juce::TextButton h3TargetButton{"H3"};
    juce::TextButton allTargetButton{"ALL"};

    // Motion-first surface: permanent motion section under the head view,
    // preset stepper in the header. Nothing covers the scene.
    SpatialMotionSection motionSection{valueTreeState};
    SpatialPresetRow presetRow{valueTreeState};
    SpatialRoutingDrawer routingDrawer{valueTreeState};

    bool perHeadHintDismissed = false;

    // Attachments declared LAST (destroyed FIRST, while components still exist)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachment;
    std::unique_ptr<juce::ParameterAttachment> tapModeAttachment;
    std::unique_ptr<juce::ParameterAttachment> delayModeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialPanel)
};
