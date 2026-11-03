// SpatialPanel: Header strip + head view + motion section + drawers.

#include "SpatialPanel.h"
#include "../../SpatialParamIDs.h"
#include "SpatialPositionEditor.h"
#include "../UIConstants.h"

// ===== SpatialPanel =====
SpatialPanel::SpatialPanel(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    // Setup Enable button
    enableButton.setButtonText("Enable");
    enableButton.setTooltip("Enable spatial positioning and rendering. Best on headphones; small head turns can "
                            "improve front/back localization.");
    enableButton.setTitle("Spatial Processing");
    enableButton.setWantsKeyboardFocus(true);
    enableButton.setExplicitFocusOrder(1);
    addAndMakeVisible(enableButton);

    headView.setExplicitFocusOrder(2);

    // Listen to enable button state to control the view's idle animation
    enableButton.onClick = [this]() { headView.setRadarEnabled(enableButton.getToggleState()); };

    // Setup the head view (handles its own mouse interaction)
    addAndMakeVisible(headView);
    headView.setTitle("Spatial Head View");
    headView.onPositionReadoutClicked = [this] { showPositionEditor(); };

    // ===== Header: preset stepper (presets-first on-ramp) =====
    addAndMakeVisible(presetRow);
    presetRow.onLibraryMenuVisibility = [this](bool open)
    {
        headView.setLibraryMenuOpen(open);
        headView.updateGLAttachment();
    };

    // ===== Mode toggle (PER-HEAD; off = Linked, the 95% case) =====
    perTapModeButton.setClickingTogglesState(false);
    perTapModeButton.setTooltip(
        "Each echo head (H1/H2/H3) gets its own spatial position. Off: all echoes share one position.");
    perTapModeButton.setTitle("Per-Head Mode");
    perTapModeButton.setLookAndFeel(&compactButtonLookAndFeel);
    addAndMakeVisible(perTapModeButton);
    perTapModeButton.onClick = [this]()
    {
        if (tapModeAttachment)
            tapModeAttachment->setValueAsCompleteGesture(headView.isPerTapMode() ? 0.0f : 1.0f);
    };

    // ===== Tap chips (children of the head view so the GL layer renders
    // them - a software-painted sibling over the GL region is invisible) =====
    auto setupTargetButton =
        [&](juce::TextButton& b, SpatialHeadView::Target target, const juce::String& tip, const juce::String& title)
    {
        b.setClickingTogglesState(false);
        b.setTooltip(tip);
        b.setTitle(title);
        b.setLookAndFeel(&compactButtonLookAndFeel);
        b.onClick = [this, target]() { headView.setEditTarget(target); };
        headView.addAndMakeVisible(b);
    };
    setupTargetButton(h1TargetButton, SpatialHeadView::Target::H1, "Edit H1's spatial position. Shortcut: 1",
                      "Edit head 1");
    setupTargetButton(h2TargetButton, SpatialHeadView::Target::H2, "Edit H2's spatial position. Shortcut: 2",
                      "Edit head 2");
    setupTargetButton(h3TargetButton, SpatialHeadView::Target::H3, "Edit H3's spatial position. Shortcut: 3",
                      "Edit head 3");
    setupTargetButton(allTargetButton, SpatialHeadView::Target::All,
                      "Move all active heads together, preserving their spacing. Shortcut: A", "Edit all active heads");

    headView.onTargetChanged = [this](SpatialHeadView::Target) { syncTargetButtons(); };

    addAndMakeVisible(motionSection);
    addAndMakeVisible(routingDrawer);
    motionSection.onLayoutChanged = [this]
    {
        resized();
        repaint();
    };
    routingDrawer.onLayoutChanged = [this]
    {
        resized();
        repaint();
    };

    // Lock state queries read the SpatialLock* parameters directly (the lock
    // toggles themselves live in the exact-entry popover).
    headView.isAzimuthLockedFn = [this]() { return isLockParamSet(SpatialIDs::binaural::kLockAzimuth); };
    headView.isDistanceLockedFn = [this]() { return isLockParamSet(SpatialIDs::binaural::kLockDistance); };
    headView.isElevationLockedFn = [this]() { return isLockParamSet(SpatialIDs::binaural::kLockElevation); };

    // Component names identify controls in layout diagnostics.
    enableButton.setName("enableButton");
    headView.setName("headView");

    // Create attachments LAST (after all component setup)
    enableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        valueTreeState, SpatialIDs::binaural::kEnabled, enableButton);

    // Mode parameter drives the view + button states (and survives automation).
    if (auto* modeParam = valueTreeState.getParameter("HeadPositionMode"))
    {
        tapModeAttachment = std::make_unique<juce::ParameterAttachment>(
            *modeParam,
            [this](float value)
            {
                const bool independent = value > 0.5f;
                headView.setPerTapMode(independent);
                if (independent && !perHeadHintDismissed)
                {
                    setPerHeadHintDismissed(true);
                    if (onPerHeadHintDismissed)
                        onPerHeadHintDismissed();
                }
                headView.setDiscoveryCueVisible(!perHeadHintDismissed);
                syncTargetButtons();
            },
            nullptr);
        tapModeAttachment->sendInitialUpdate();
    }

    // Echo Mode is the source of truth for which head positions can currently
    // affect the audible delay output. Position state is preserved in APVTS;
    // only the edit/render affordances change here.
    if (auto* delayParam = valueTreeState.getParameter("DelaySetting"))
    {
        delayModeAttachment = std::make_unique<juce::ParameterAttachment>(
            *delayParam,
            [this](float value)
            {
                const auto previousTarget = headView.getEditTarget();
                headView.setActiveTapMask(DelayHeadActivation::activeHeadsForDelayMode(juce::roundToInt(value)));
                if (positionEditorBox != nullptr &&
                    (!headView.canEditCurrentTarget() || headView.getEditTarget() != previousTarget))
                {
                    positionEditorBox->setLookAndFeel(nullptr);
                    positionEditorBox->dismiss();
                    positionEditorBox = nullptr;
                }
                syncTargetButtons();
            },
            nullptr);
        delayModeAttachment->sendInitialUpdate();
    }

    // Set initial enabled state for the idle animation
    headView.setRadarEnabled(enableButton.getToggleState());

    syncTargetButtons();
}

SpatialPanel::~SpatialPanel()
{
    if (positionEditorBox != nullptr)
    {
        // Unset synchronously: dismiss() deletes the desktop box a message
        // later, after calloutLookAndFeel is already gone.
        positionEditorBox->setLookAndFeel(nullptr);
        positionEditorBox->dismiss();
    }

    for (auto* b : {&perTapModeButton, &h1TargetButton, &h2TargetButton, &h3TargetButton, &allTargetButton})
        b->setLookAndFeel(nullptr);
}

void SpatialPanel::setPerHeadHintDismissed(bool dismissed)
{
    perHeadHintDismissed = dismissed;
    headView.setDiscoveryCueVisible(!perHeadHintDismissed);
}

juce::Rectangle<int> SpatialPanel::getWalkthroughEnableScreenBounds() const
{
    return enableButton.getScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughPresetScreenBounds() const
{
    return presetRow.getWalkthroughScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughModeScreenBounds() const
{
    return perTapModeButton.getScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughHeadSelectorScreenBounds() const
{
    return h1TargetButton.getScreenBounds()
        .getUnion(h2TargetButton.getScreenBounds())
        .getUnion(h3TargetButton.getScreenBounds())
        .getUnion(allTargetButton.getScreenBounds());
}

juce::Rectangle<int> SpatialPanel::getWalkthroughHeadViewScreenBounds() const
{
    return headView.getScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughPositionReadoutScreenBounds() const
{
    return headView.localAreaToGlobal(headView.getPositionReadoutBounds());
}

juce::Rectangle<int> SpatialPanel::getWalkthroughPositionEditorScreenBounds() const
{
    return positionEditorBox != nullptr ? positionEditorBox->getScreenBounds() : juce::Rectangle<int>();
}

void SpatialPanel::showWalkthroughPresetMenu()
{
    presetRow.showWalkthroughMenu();
}

bool SpatialPanel::isWalkthroughPositionEditorVisible() const
{
    return positionEditorBox != nullptr && positionEditorBox->isShowing();
}

void SpatialPanel::setWalkthroughPositionEditorVisible(bool visible)
{
    if (visible)
    {
        if (positionEditorBox == nullptr)
            showPositionEditor();
        return;
    }

    if (positionEditorBox != nullptr)
    {
        positionEditorBox->setLookAndFeel(nullptr);
        positionEditorBox->dismiss();
        positionEditorBox = nullptr;
    }
}

void SpatialPanel::revealWalkthroughDistanceReferences()
{
    headView.revealWalkthroughDistanceReferences();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughBlendQualityScreenBounds() const
{
    return routingDrawer.getWalkthroughQualityScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughBlendChainScreenBounds() const
{
    return routingDrawer.getWalkthroughChainScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughBlendWetScreenBounds() const
{
    return routingDrawer.getWalkthroughWetScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughBlendWidthScreenBounds() const
{
    return routingDrawer.getWalkthroughWidthScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughBlendCrossoverScreenBounds() const
{
    return routingDrawer.getWalkthroughCrossoverScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionPatternScreenBounds() const
{
    return motionSection.getWalkthroughPatternScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionRateScreenBounds() const
{
    return motionSection.getWalkthroughRateScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionSyncScreenBounds() const
{
    return motionSection.getWalkthroughSyncScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionDivisionControlScreenBounds() const
{
    return motionSection.getWalkthroughDivisionControlScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionDepthScreenBounds() const
{
    return motionSection.getWalkthroughDepthScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionFineTuneButtonScreenBounds() const
{
    return motionSection.getWalkthroughFineTuneButtonScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionChoreographyScreenBounds() const
{
    return motionSection.getWalkthroughChoreographyScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionSpreadScreenBounds() const
{
    return motionSection.getWalkthroughSpreadScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionRetriggerScreenBounds() const
{
    return motionSection.getWalkthroughRetriggerScreenBounds();
}

juce::Rectangle<int> SpatialPanel::getWalkthroughMotionDivisionMenuScreenBounds() const
{
    return motionSection.getWalkthroughDivisionMenuScreenBounds();
}

bool SpatialPanel::isWalkthroughMotionDivisionMenuVisible() const
{
    return motionSection.isWalkthroughDivisionMenuVisible();
}

void SpatialPanel::showWalkthroughMotionDivisionMenu()
{
    motionSection.showWalkthroughDivisionMenu();
}

void SpatialPanel::dismissWalkthroughMotionDivisionMenu()
{
    motionSection.dismissWalkthroughDivisionMenu();
}

void SpatialPanel::setWalkthroughMotionNavigationComponent(juce::Component* component)
{
    motionSection.setWalkthroughNavigationComponent(component);
}

bool SpatialPanel::isLockParamSet(const char* paramId) const
{
    if (auto* raw = valueTreeState.getRawParameterValue(paramId))
        return raw->load() >= 0.5f;
    return false;
}

void SpatialPanel::showPositionEditor()
{
    if (!headView.canEditCurrentTarget())
        return;

    // Re-click on the readout while open acts as a toggle.
    if (positionEditorBox != nullptr)
    {
        positionEditorBox->dismiss();
        positionEditorBox = nullptr;
        return;
    }

    auto editor = std::make_unique<SpatialPositionEditor>(valueTreeState);

    editor->getAxisValue = [safe = juce::Component::SafePointer<SpatialPanel>(this)](int axis) -> float
    {
        if (safe == nullptr)
            return 0.0f;
        auto& view = safe->headView;
        return axis == 0 ? view.getTargetAzimuth() : axis == 1 ? view.getTargetElevation() : view.getTargetDistance();
    };
    editor->onAxisEdit = [safe = juce::Component::SafePointer<SpatialPanel>(this)](int axis, float value)
    {
        if (safe != nullptr)
            safe->applyNumericEdit(axis, value);
    };

    // Desktop-parented (nullptr): an editor-child callout is composited under
    // the GL head view. A desktop
    // CallOutBox takes its anchor in SCREEN coordinates and floats above the
    // GL layer, so the head view stays live while values are typed.
    const auto anchor = headView.localAreaToGlobal(headView.getPositionReadoutBounds());
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(editor), anchor, nullptr);
    box.setLookAndFeel(&calloutLookAndFeel);
    positionEditorBox = &box;
}

void SpatialPanel::applyNumericEdit(int axis, float value)
{
    auto& view = headView;

    if (!view.canEditCurrentTarget())
        return;

    if (!view.isPerTapMode())
    {
        // Global/Linked path: per-axis callbacks bracketed by one gesture.
        const SpatialHeadView::GestureMask mask = axis == 0   ? SpatialHeadView::gestureAzimuth
                                                  : axis == 1 ? SpatialHeadView::gestureElevation
                                                              : SpatialHeadView::gestureDistance;
        float clamped = 0.0f;
        if (axis == 0)
        {
            clamped = juce::jlimit(-180.0f, 180.0f, value);
            view.setAzimuth(clamped);
        }
        if (axis == 1)
        {
            clamped = juce::jlimit(-90.0f, 90.0f, value);
            view.setElevation(clamped);
        }
        if (axis == 2)
        {
            clamped = juce::jlimit(0.1f, 10.0f, value);
            view.setDistance(clamped);
        }
        if (view.onGestureBegin)
            view.onGestureBegin(mask);
        if (axis == 0 && view.onAzimuthChanged)
            view.onAzimuthChanged(clamped);
        if (axis == 1 && view.onElevationChanged)
            view.onElevationChanged(clamped);
        if (axis == 2 && view.onDistanceChanged)
            view.onDistanceChanged(clamped);
        if (view.onGestureEnd)
            view.onGestureEnd(mask);
        return;
    }

    // Per-tap path: numeric entry edits the primary tap (ALL anchors on the
    // first active head).
    const int tap = juce::jlimit(0, 2, view.primarySlotForTarget() - 1);
    float az = view.getTargetAzimuth();
    float el = view.getTargetElevation();
    float dist = view.getTargetDistance();
    if (axis == 0)
        az = juce::jlimit(-180.0f, 180.0f, value);
    if (axis == 1)
        el = juce::jlimit(-90.0f, 90.0f, value);
    if (axis == 2)
        dist = juce::jlimit(0.1f, 10.0f, value);
    view.setTapPosition(tap, az, el, dist);
    if (view.onTapPositionChanged)
        view.onTapPositionChanged(tap, az, el, dist);
}

void SpatialPanel::syncTargetButtons()
{
    const bool perTap = headView.isPerTapMode();
    const auto target = headView.getEditTarget();

    perTapModeButton.setToggleState(perTap, juce::dontSendNotification);

    // Tap chips stay visible. Linked mode locks all of them; Per Head mode
    // additionally locks heads that Echo Mode does not route to the output.
    const juce::String lockedTooltip = "Enable PER-HEAD mode to edit individual head positions.";
    auto syncHead = [&](juce::TextButton& button, int tapIndex, SpatialHeadView::Target buttonTarget,
                        const juce::String& activeTooltip)
    {
        const bool active = headView.isTapActive(tapIndex);
        button.setVisible(true);
        button.setEnabled(perTap && active);
        button.setAlpha(!perTap ? 0.5f : active ? 1.0f : 0.3f);
        button.setTooltip(!perTap  ? lockedTooltip
                          : active ? activeTooltip
                                   : "H" + juce::String(tapIndex + 1) + " is inactive in the current Echo Mode.");
        button.setToggleState(perTap && active && target == buttonTarget, juce::dontSendNotification);
    };

    syncHead(h1TargetButton, 0, SpatialHeadView::Target::H1, "Edit H1's spatial position. Shortcut: 1");
    syncHead(h2TargetButton, 1, SpatialHeadView::Target::H2, "Edit H2's spatial position. Shortcut: 2");
    syncHead(h3TargetButton, 2, SpatialHeadView::Target::H3, "Edit H3's spatial position. Shortcut: 3");

    const bool anyActive =
        DelayHeadActivation::anyActive({headView.isTapActive(0), headView.isTapActive(1), headView.isTapActive(2)});
    allTargetButton.setVisible(true);
    allTargetButton.setEnabled(perTap && anyActive);
    allTargetButton.setAlpha(!perTap ? 0.5f : anyActive ? 1.0f : 0.3f);
    allTargetButton.setTooltip(!perTap     ? lockedTooltip
                               : anyActive ? "Move all active heads together, preserving their spacing. Shortcut: A"
                                           : "No echo heads are active in Reverb Only mode.");
    allTargetButton.setToggleState(perTap && anyActive && target == SpatialHeadView::Target::All,
                                   juce::dontSendNotification);
}

void SpatialPanel::paint(juce::Graphics& g)
{
    auto green = CRTLookAndFeel::getTerminalGreen();

    // CRT bezel around the head viewport: bright corner accents + thin frame,
    // so the view reads as a framed instrument screen rather than a void.
    // Deliberately the only box on the panel.
    const auto frame = headView.getBounds().toFloat().expanded(2.0f);
    g.setColour(green.withAlpha(0.25f));
    g.drawRect(frame, 1.0f);
    g.setColour(green.withAlpha(0.6f));
    CRTLookAndFeel::drawCornerAccents(g, frame, 9.0f, 1.6f);
}

void SpatialPanel::resized()
{
    auto bounds = getLocalBounds();
    const int padding = UIConstants::panelPadding;

    // ===== Header row: ENABLE | preset stepper (centred) | PER-HEAD =====
    constexpr int headerY = 8;
    constexpr int headerH = 24;
    // 84px: the 24px-tall toggle grows its tick box, and 68 truncated "ENABLE"
    // to "ENAB...".
    constexpr int enableW = 84;
    enableButton.setBounds(padding, headerY, enableW, headerH);
    const int modeW = 64;
    perTapModeButton.setBounds(bounds.getRight() - padding - modeW, headerY, modeW, headerH);

    const int presetW = juce::jmin(380, bounds.getWidth() - 2 * (padding + enableW + modeW + 16));
    presetRow.setBounds(bounds.getCentreX() - presetW / 2, headerY, presetW, headerH);

    bounds.removeFromTop(headerY + headerH + 8);

    // ===== Bottom-up: routing drawer, then motion section =====
    auto routingArea = bounds.removeFromBottom(routingDrawer.getPreferredHeight());
    routingDrawer.setBounds(routingArea.reduced(padding, 0));
    bounds.removeFromBottom(6);

    auto motionArea = bounds.removeFromBottom(motionSection.getPreferredHeight());
    motionSection.setBounds(motionArea.reduced(padding, 0));
    bounds.removeFromBottom(6);

    // ===== Head view: everything that remains, full width =====
    headView.setBounds(bounds.reduced(padding, 2));

    // Tap chips live INSIDE the head view (children of the GL-attached
    // component, positioned in ITS local coordinates): as software-painted
    // siblings they would be composited UNDER the GL layer and invisible.
    // Chip row at the 24px minimum hit target.
    auto chipRow = headView.getLocalBounds().reduced(8).removeFromTop(24);
    auto placeChip = [&chipRow](juce::TextButton& b, int width)
    {
        b.setBounds(chipRow.removeFromLeft(width));
        chipRow.removeFromLeft(6);
    };
    placeChip(h1TargetButton, 30);
    placeChip(h2TargetButton, 30);
    placeChip(h3TargetButton, 30);
    placeChip(allTargetButton, 38);
}
