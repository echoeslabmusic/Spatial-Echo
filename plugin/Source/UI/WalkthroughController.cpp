// WalkthroughController: Editor-side driver of the interactive tour (step surfaces, spotlight targets, completion).

#include "WalkthroughController.h"
#include "../PluginEditor.h"
#include "../SpatialParamIDs.h"
#include <cmath>

namespace
{
// Every parameter a completion check reads. Captured when a step begins and
// compared by ID when the step asks whether its action happened.
constexpr const char* kTrackedParameterIds[] = {
    "TapeFlavor",
    "TapeDrive",
    "TapeNoiseEnabled",
    "DelaySetting",
    "RepeatRate",
    "Intensity",
    "EchoLevel",
    "PingPong",
    "TempoSync",
    "Head1NoteDivision",
    "Head2NoteDivision",
    "Head3NoteDivision",
    "EchoSend",
    "HeadRatioPreset",
    "AutoPanic",
    "ReverbEngine",
    "ReverbSize",
    "ReverbDecay",
    "ReverbTone",
    "ReverbCharacter",
    "ReverbMod",
    "ReverbWidth",
    "ReverbPreDelay",
    "ReverbDuckAmount",
    "ReverbLevel",
    "ReverbRouting",
    SpatialIDs::binaural::kEnabled,
    "HeadPositionMode",
    SpatialIDs::binaural::kAzimuth,
    SpatialIDs::binaural::kElevation,
    SpatialIDs::binaural::kDistance,
    SpatialIDs::taps::kHead1Azimuth,
    SpatialIDs::taps::kHead1Elevation,
    SpatialIDs::taps::kHead1Distance,
    SpatialIDs::taps::kHead2Azimuth,
    SpatialIDs::taps::kHead2Elevation,
    SpatialIDs::taps::kHead2Distance,
    SpatialIDs::taps::kHead3Azimuth,
    SpatialIDs::taps::kHead3Elevation,
    SpatialIDs::taps::kHead3Distance,
    SpatialIDs::binaural::kLockAzimuth,
    SpatialIDs::binaural::kLockElevation,
    SpatialIDs::binaural::kLockDistance,
    SpatialIDs::binaural::kWidth,
    SpatialIDs::binaural::kCrossover,
    SpatialIDs::routing::kSpatialRouting,
    SpatialIDs::binaural::kWetMix,
    SpatialIDs::binaural::kQuality,
    SpatialIDs::motion::kPattern,
    SpatialIDs::motion::kRate,
    SpatialIDs::motion::kSync,
    SpatialIDs::motion::kDivision,
    SpatialIDs::motion::kLongDivision,
    SpatialIDs::motion::kLongVariantDivision,
    SpatialIDs::motion::kDepth,
    SpatialIDs::motion::kRetrig,
    SpatialIDs::motion::kChoreo,
    SpatialIDs::motion::kSpread,
    "Mix",
    "MixLock",
    "OutputGain",
};

// The currently open PopupMenu window, if any.
juce::Component* findOpenPopupMenu()
{
    for (int i = 0; i < juce::Component::getNumCurrentlyModalComponents(); ++i)
        if (auto* modal = juce::Component::getCurrentlyModalComponent(i))
            if (modal->getName() == "menu" && modal->isShowing())
                return modal;
    return nullptr;
}
}  // namespace

WalkthroughController::WalkthroughController(SpatialEchoAudioProcessorEditor& owner)
    : editor(owner)
    , walkthrough(owner.interactiveWalkthrough)
{
    walkthrough.setTargetResolver([this](Target target, ExpandedTarget expandedTarget, SpotlightDetail spotlightDetail)
                                  { return resolveTarget(target, expandedTarget, spotlightDetail); });
    walkthrough.setCoveringSurfaceResolver([this](ExpandedTarget expandedTarget)
                                           { return resolveCoveringSurface(expandedTarget); });
    walkthrough.setCompletionResolver([this](CompletionSignal completionSignal)
                                      { return isActionComplete(completionSignal); });
    walkthrough.onStepChanged = [this](Target target, ExpandedTarget expandedTarget, SpotlightDetail spotlightDetail)
    { prepareStep(target, expandedTarget, spotlightDetail); };
    walkthrough.onDismissed = [this](InteractiveWalkthrough::Dismissal dismissal) { handleDismissal(dismissal); };
    walkthrough.onActiveChanged = [this](bool) { editor.updateHeadViewGLForOverlays(); };
}

WalkthroughController::~WalkthroughController()
{
    detach();
}

void WalkthroughController::detach()
{
    walkthrough.onDismissed = nullptr;
    walkthrough.onActiveChanged = nullptr;
    walkthrough.onStepChanged = nullptr;
    walkthrough.setTargetResolver({});
    walkthrough.setCoveringSurfaceResolver({});
    walkthrough.setCompletionResolver({});
}

//==============================================================================
// Launch and dismissal
//==============================================================================

void WalkthroughController::scheduleAutomaticStartIfEligible()
{
    // Some wrappers make the editor visible before attaching its native peer,
    // so visibilityChanged() can see isShowing() too early; the editor timer
    // calls this again as a wrapper-independent first-show fallback.
    if (!editor.isShowing() || autoStartEvaluated || autoStartScheduled)
        return;

    autoStartScheduled = true;
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeEditor(&editor);
    juce::MessageManager::callAsync(
        [safeEditor, this]
        {
            if (safeEditor == nullptr)
                return;

            autoStartScheduled = false;
            if (!editor.isShowing() || autoStartEvaluated)
                return;

            autoStartEvaluated = true;
            start(true);
        });
}

void WalkthroughController::start(bool automatic)
{
    auto& launchGate = EditorPreferences::getProcessLaunchGate();
    if (automatic)
    {
        if (!launchGate.tryClaim(editor.ensureEditorPreferences().hasSeenWalkthrough()))
            return;
    }
    else
    {
        // Manual replay ignores the preference, but it still claims this
        // process so a deferred first-show callback cannot reopen the tour.
        (void)launchGate.tryClaim(false);
    }

    // A manual replay can be selected before the deferred first-show callback
    // runs. The automatic path still claims this host process, but it must not
    // reset the active tour to Welcome.
    if (automatic && walkthrough.isVisible())
        return;

    if (editor.helpOverlay.isVisible())
        editor.helpOverlay.setVisible(false);
    closeTourSurfaces();

    walkthrough.setBounds(editor.getLocalBounds());
    walkthrough.start();
    walkthrough.toFront(true);
    editor.updateHeadViewGLForOverlays();
}

void WalkthroughController::handleDismissal(InteractiveWalkthrough::Dismissal dismissal)
{
    if (dismissal == InteractiveWalkthrough::Dismissal::dontShowAgain ||
        dismissal == InteractiveWalkthrough::Dismissal::completed)
    {
        // A failed write must not trap the user in the overlay. The in-process
        // launch gate still prevents another automatic prompt in this host.
        (void)editor.ensureEditorPreferences().markWalkthroughSeen();
    }

    closeTourSurfaces();
    editor.updateHeadViewGLForOverlays();
}

// Closes every drawer, menu and popover a step may have opened.
void WalkthroughController::closeTourSurfaces()
{
    if (editor.presetBrowser.isVisible())
        editor.presetBrowser.dismiss();
    if (isAnyMenuVisible())
        walkthrough.dismissAttachedModalComponent();
    tapeMenuVisible = false;
    appMenuVisible = false;
    walkthrough.setExternalPopupActive(false);
    editor.echoEngineDropdown.hideDropdown();
    editor.reverbDropdown.hideDropdown();
    editor.dismissDivisionMenu();
    dismissSpatialPresetMenu();
    editor.spatialPanel.setWalkthroughPositionEditorVisible(false);
    editor.spatialPanel.dismissWalkthroughMotionDivisionMenu();
    editor.spatialPanel.setWalkthroughBlendExpanded(false);
    editor.spatialPanel.setWalkthroughMotionFineTuneVisible(false);
}

void WalkthroughController::dismissSpatialPresetMenu()
{
    if (spatialPresetMenu == nullptr)
        return;
    walkthrough.detachFromModalComponent();
    spatialPresetMenu->exitModalState(0);
    spatialPresetMenu = nullptr;
}

void WalkthroughController::timerTick()
{
    scheduleAutomaticStartIfEligible();

    if (!walkthrough.isVisible())
        return;

    // PopupMenu windows are editor children. Leave an active Flavor or
    // wordmark menu above the overlay so its rows remain operable.
    if (!isAnyMenuVisible())
        walkthrough.toFront(false);
    walkthrough.refreshActionState();
}

void WalkthroughController::setTapeMenuVisible(bool visible)
{
    tapeMenuVisible = visible;
    walkthrough.setExternalPopupActive(isAnyMenuVisible());
}

void WalkthroughController::setAppMenuVisible(bool visible)
{
    appMenuVisible = visible;
    walkthrough.setExternalPopupActive(isAnyMenuVisible());
}

int WalkthroughController::beginTapeMenu()
{
    return ++tapeMenuGeneration;
}

//==============================================================================
// Step preparation
//==============================================================================

void WalkthroughController::prepareStep(Target target, ExpandedTarget expandedTarget, SpotlightDetail spotlightDetail)
{
    juce::ignoreUnused(target);

    if (editor.helpOverlay.isVisible())
        editor.helpOverlay.setVisible(false);

    editor.presetBrowser.dismissLibraryMenu();

    const bool keepTapeMenuOpen =
        expandedTarget == ExpandedTarget::tapeMenu && tapeMenuVisible && walkthrough.hasActiveAttachedModalComponent();

    if (isAnyMenuVisible() && expandedTarget != ExpandedTarget::tapeMenu &&
        expandedTarget != ExpandedTarget::wordmarkMenu)
    {
        walkthrough.dismissAttachedModalComponent();
    }
    else
    {
        walkthrough.detachFromModalComponent();
    }

    if (expandedTarget != ExpandedTarget::presetBrowser && editor.presetBrowser.isVisible())
        editor.presetBrowser.dismiss();
    if (expandedTarget != ExpandedTarget::echoEngineDrawer)
        editor.echoEngineDropdown.hideDropdown();
    if (expandedTarget != ExpandedTarget::reverbDrawer)
        editor.reverbDropdown.hideDropdown();
    if (expandedTarget != ExpandedTarget::delayDivisionMenu)
        editor.dismissDivisionMenu();
    if (expandedTarget != ExpandedTarget::spatialPresetMenu)
        dismissSpatialPresetMenu();
    if (expandedTarget != ExpandedTarget::spatialPositionEditor)
        editor.spatialPanel.setWalkthroughPositionEditorVisible(false);
    if (expandedTarget != ExpandedTarget::motionDivisionMenu)
        editor.spatialPanel.dismissWalkthroughMotionDivisionMenu();
    editor.spatialPanel.setWalkthroughBlendExpanded(expandedTarget == ExpandedTarget::spatialBlendDrawer);
    editor.spatialPanel.setWalkthroughMotionFineTuneVisible(expandedTarget == ExpandedTarget::motionFineTune);
    if (!keepTapeMenuOpen)
        tapeMenuVisible = false;
    appMenuVisible = false;
    walkthrough.setExternalPopupActive(keepTapeMenuOpen);

    baselinePresetName = editor.currentPresetName;
    baselinePresetType = static_cast<int>(editor.currentPresetType);
    baselinePresetFilter =
        editor.presetBrowser.isVisible() ? editor.presetBrowser.getWalkthroughFilterText() : juce::String{};
    baselineValues = readTrackedValues();

    if (spotlightDetail == SpotlightDetail::spatialDistanceView)
        editor.spatialPanel.revealWalkthroughDistanceReferences();

    // Surfaces that must open after the current event finishes are opened
    // asynchronously, and only if the tour still shows the same step. Popup
    // menus position themselves, so they skip the editor-showing check.
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeEditor(&editor);
    auto openLater = [safeEditor, this, expandedTarget](bool requireShowing, std::function<void()> openSurface)
    {
        juce::MessageManager::callAsync(
            [safeEditor, this, expandedTarget, requireShowing, open = std::move(openSurface)]
            {
                if (safeEditor == nullptr || (requireShowing && !editor.isShowing()) ||
                    !walkthrough.isShowingExpandedTarget(expandedTarget))
                    return;
                open();
            });
    };

    switch (expandedTarget)
    {
        case ExpandedTarget::presetBrowser:
            if (!editor.presetBrowser.isVisible())
                editor.showPresetBrowser();
            if (spotlightDetail == SpotlightDetail::presetUserPresets)
                editor.presetBrowser.revealWalkthroughUserPresets();
            else if (spotlightDetail == SpotlightDetail::presetList || spotlightDetail == SpotlightDetail::presetIntent)
                editor.presetBrowser.revealWalkthroughLoadedPreset();
            break;

        case ExpandedTarget::tapeMenu:
            if (keepTapeMenuOpen)
                walkthrough.attachToCurrentPopupMenu();
            else if (editor.isShowing())
                openLater(false, [this] { editor.showTapeFlavorMenu(); });
            break;

        case ExpandedTarget::delayDivisionMenu:
            if (editor.isShowing())
                openLater(true, [this] { editor.showHeadSubdivisionMenu(1, true); });
            break;

        case ExpandedTarget::echoEngineDrawer:
            if (editor.isShowing())
                openLater(true,
                          [this]
                          {
                              editor.echoEngineDropdown.showDropdown();
                              editor.echoEngineDropdown.setWalkthroughNavigationComponent(&walkthrough);
                              walkthrough.attachToCurrentModalComponent();
                          });
            break;

        case ExpandedTarget::reverbDrawer:
            if (editor.isShowing())
                openLater(true,
                          [this]
                          {
                              editor.reverbDropdown.showDropdown();
                              editor.reverbDropdown.setWalkthroughNavigationComponent(&walkthrough);
                              walkthrough.attachToCurrentModalComponent();
                          });
            break;

        case ExpandedTarget::spatialBlendDrawer:
        case ExpandedTarget::motionFineTune:
            break;

        case ExpandedTarget::spatialPositionEditor:
            editor.spatialPanel.setWalkthroughPositionEditorVisible(true);
            walkthrough.attachToCurrentModalComponent();
            break;

        case ExpandedTarget::spatialPresetMenu:
            if (editor.isShowing())
                openLater(true,
                          [this]
                          {
                              editor.spatialPanel.showWalkthroughPresetMenu();
                              walkthrough.setExternalPopupActive(true);
                              walkthrough.attachToCurrentPopupMenu();
                              for (int i = juce::Component::getNumCurrentlyModalComponents() - 1; i >= 0; --i)
                                  if (auto* modal = juce::Component::getCurrentlyModalComponent(i))
                                      if (modal->getName() == "menu" && modal->isShowing())
                                      {
                                          spatialPresetMenu = modal;
                                          break;
                                      }
                          });
            break;

        case ExpandedTarget::motionDivisionMenu:
            if (editor.isShowing())
            {
                editor.spatialPanel.showWalkthroughMotionDivisionMenu();
                editor.spatialPanel.setWalkthroughMotionNavigationComponent(&walkthrough);
                walkthrough.attachToCurrentModalComponent();
            }
            break;

        case ExpandedTarget::wordmarkMenu:
            if (editor.isShowing())
                openLater(false, [this] { editor.showAppMenu(); });
            break;

        case ExpandedTarget::none:
            break;
    }
}

//==============================================================================
// Spotlight targets
//==============================================================================

std::optional<juce::Rectangle<int>> WalkthroughController::resolveTarget(Target target, ExpandedTarget expandedTarget,
                                                                         SpotlightDetail spotlightDetail) const
{
    auto screen = resolveExpandedTarget(expandedTarget, spotlightDetail);
    if (!screen.has_value())
        screen = resolveSurfaceTarget(target, spotlightDetail);

    if (!screen.has_value() || screen->isEmpty())
        return std::nullopt;
    return editor.getLocalArea(nullptr, *screen);
}

// Screen bounds of an open drawer, menu or popover, or nullopt while the
// step's expanded surface is not (yet) showing.
std::optional<juce::Rectangle<int>> WalkthroughController::resolveExpandedTarget(ExpandedTarget expandedTarget,
                                                                                 SpotlightDetail spotlightDetail) const
{
    auto& browser = editor.presetBrowser;
    auto& spatial = editor.spatialPanel;
    juce::Rectangle<int> screen;

    switch (expandedTarget)
    {
        case ExpandedTarget::presetBrowser:
            if (browser.isVisible())
            {
                if (spotlightDetail == SpotlightDetail::presetCategories)
                    screen = browser.getWalkthroughCategoryScreenBounds();
                else if (spotlightDetail == SpotlightDetail::presetList)
                    screen = browser.getWalkthroughPresetListScreenBounds();
                else if (spotlightDetail == SpotlightDetail::presetIntent)
                    screen = browser.getWalkthroughIntentScreenBounds();
                else if (spotlightDetail == SpotlightDetail::presetFilter)
                    screen = browser.getWalkthroughFilterScreenBounds();
                else if (spotlightDetail == SpotlightDetail::presetActions)
                    screen = browser.getWalkthroughActionScreenBounds();
                else if (spotlightDetail == SpotlightDetail::presetUserPresets)
                    screen = browser.getWalkthroughUserPresetScreenBounds();
                else
                    screen = browser.getWalkthroughPanelScreenBounds();
            }
            break;

        case ExpandedTarget::delayDivisionMenu:
            if (editor.divisionMenuWindow != nullptr && editor.divisionMenuWindow->isShowing())
                screen = editor.divisionMenuWindow->getScreenBounds();
            break;

        case ExpandedTarget::echoEngineDrawer:
            if (editor.echoEngineDropdown.isDropdownVisible())
            {
                using Region = EchoEngineDropdown::WalkthroughRegion;
                screen = editor.echoEngineDropdown.getWalkthroughScreenBounds(
                    spotlightDetail == SpotlightDetail::echoHeadRatio ? Region::headRatio : Region::drawer);
            }
            break;

        case ExpandedTarget::reverbDrawer:
            if (editor.reverbDropdown.isDropdownVisible())
            {
                using Region = ReverbDropdown::WalkthroughRegion;
                std::optional<Region> region;
                if (spotlightDetail == SpotlightDetail::reverbEngine)
                    region = Region::engine;
                else if (spotlightDetail == SpotlightDetail::reverbPreset)
                    region = Region::preset;
                else if (spotlightDetail == SpotlightDetail::reverbPreDelayWidth)
                    region = Region::preDelayWidth;
                else if (spotlightDetail == SpotlightDetail::reverbCharacter)
                    region = Region::character;
                else if (spotlightDetail == SpotlightDetail::reverbMod)
                    region = Region::mod;
                else if (spotlightDetail == SpotlightDetail::reverbDucking)
                    region = Region::ducking;
                else if (spotlightDetail == SpotlightDetail::reverbPin)
                    region = Region::pin;
                else if (spotlightDetail == SpotlightDetail::reverbReset)
                    region = Region::reset;

                std::optional<juce::Rectangle<int>> detailBounds;
                if (region.has_value())
                    detailBounds = editor.reverbDropdown.getWalkthroughScreenBounds(*region);
                screen = detailBounds.value_or(editor.reverbDropdown.getWalkthroughScreenBounds());
            }
            break;

        case ExpandedTarget::spatialBlendDrawer:
            if (spatial.isWalkthroughBlendExpanded())
            {
                if (spotlightDetail == SpotlightDetail::spatialWet)
                    screen = spatial.getWalkthroughBlendWetScreenBounds();
                else if (spotlightDetail == SpotlightDetail::spatialCrossover)
                    screen = spatial.getWalkthroughBlendCrossoverScreenBounds();
                else if (spotlightDetail == SpotlightDetail::spatialWidth)
                    screen = spatial.getWalkthroughBlendWidthScreenBounds();
                else if (spotlightDetail == SpotlightDetail::spatialChain)
                    screen = spatial.getWalkthroughBlendChainScreenBounds();
                else if (spotlightDetail == SpotlightDetail::spatialQuality)
                    screen = spatial.getWalkthroughBlendQualityScreenBounds();
                else
                    screen = spatial.getWalkthroughBlendScreenBounds();
            }
            break;

        case ExpandedTarget::spatialPresetMenu:
            if (spatialPresetMenu != nullptr && spatialPresetMenu->isShowing())
                screen = spatialPresetMenu->getScreenBounds();
            break;

        case ExpandedTarget::spatialPositionEditor:
            if (spatial.isWalkthroughPositionEditorVisible())
                screen = spatial.getWalkthroughPositionEditorScreenBounds();
            break;

        case ExpandedTarget::motionDivisionMenu:
            if (spatial.isWalkthroughMotionDivisionMenuVisible())
                screen = spatial.getWalkthroughMotionDivisionMenuScreenBounds();
            break;

        case ExpandedTarget::motionFineTune:
            if (spatial.isWalkthroughMotionFineTuneVisible())
            {
                if (spotlightDetail == SpotlightDetail::motionChoreo)
                    screen = spatial.getWalkthroughMotionChoreographyScreenBounds();
                else if (spotlightDetail == SpotlightDetail::motionSpread)
                    screen = spatial.getWalkthroughMotionSpreadScreenBounds();
                else if (spotlightDetail == SpotlightDetail::motionRetrig)
                    screen = spatial.getWalkthroughMotionRetriggerScreenBounds();
                else
                    screen = spatial.getWalkthroughMotionScreenBounds();
            }
            break;

        case ExpandedTarget::tapeMenu:
            if (auto* menu = findOpenPopupMenu())
            {
                const int flavorCount = editor.tapeFlavorBox.getNumItems();
                if (spotlightDetail == SpotlightDetail::tapeFlavors)
                {
                    for (int childIndex = 0; childIndex < flavorCount && childIndex < menu->getNumChildComponents();
                         ++childIndex)
                    {
                        const auto childBounds = menu->getChildComponent(childIndex)->getScreenBounds();
                        screen = screen.isEmpty() ? childBounds : screen.getUnion(childBounds);
                    }
                }
                else if (spotlightDetail == SpotlightDetail::tapeNoise)
                {
                    const int noiseItemIndex = flavorCount + 1;  // separator precedes Noise
                    if (noiseItemIndex < menu->getNumChildComponents())
                        screen = menu->getChildComponent(noiseItemIndex)->getScreenBounds();
                }
                else
                {
                    screen = menu->getScreenBounds();
                }
            }
            break;

        case ExpandedTarget::wordmarkMenu:
            if (auto* menu = findOpenPopupMenu())
                screen = menu->getScreenBounds();
            break;

        case ExpandedTarget::none:
            break;
    }

    if (screen.isEmpty())
        return std::nullopt;
    return screen;
}

// Local bounds of a step's open surface when it draws above the tour: a
// drawer, division menu or callout in its own window, or a popup menu. The
// coach card keeps clear of all of it, not only the spotlit control inside it.
std::optional<juce::Rectangle<int>> WalkthroughController::resolveCoveringSurface(ExpandedTarget expandedTarget) const
{
    juce::Rectangle<int> screen;

    switch (expandedTarget)
    {
        case ExpandedTarget::echoEngineDrawer:
            screen = editor.echoEngineDropdown.getOpenDrawerScreenBounds();
            break;

        case ExpandedTarget::reverbDrawer:
            screen = editor.reverbDropdown.getOpenDrawerScreenBounds();
            break;

        case ExpandedTarget::delayDivisionMenu:
            if (editor.divisionMenuWindow != nullptr && editor.divisionMenuWindow->isShowing())
                screen = editor.divisionMenuWindow->getScreenBounds();
            break;

        case ExpandedTarget::motionDivisionMenu:
            if (editor.spatialPanel.isWalkthroughMotionDivisionMenuVisible())
                screen = editor.spatialPanel.getWalkthroughMotionDivisionMenuScreenBounds();
            break;

        case ExpandedTarget::spatialPositionEditor:
            if (editor.spatialPanel.isWalkthroughPositionEditorVisible())
                screen = editor.spatialPanel.getWalkthroughPositionEditorScreenBounds();
            break;

        case ExpandedTarget::spatialPresetMenu:
            if (spatialPresetMenu != nullptr && spatialPresetMenu->isShowing())
                screen = spatialPresetMenu->getScreenBounds();
            break;

        case ExpandedTarget::tapeMenu:
        case ExpandedTarget::wordmarkMenu:
            if (auto* menu = findOpenPopupMenu())
                screen = menu->getScreenBounds();
            break;

        // Drawn inside the editor, beneath the tour.
        case ExpandedTarget::none:
        case ExpandedTarget::presetBrowser:
        case ExpandedTarget::spatialBlendDrawer:
        case ExpandedTarget::motionFineTune:
            break;
    }

    if (screen.isEmpty())
        return std::nullopt;
    return editor.getLocalArea(nullptr, screen);
}

// Screen bounds of the always-visible control a step points at.
std::optional<juce::Rectangle<int>> WalkthroughController::resolveSurfaceTarget(Target target,
                                                                                SpotlightDetail spotlightDetail) const
{
    auto& spatial = editor.spatialPanel;

    switch (target)
    {
        case Target::none:
            return std::nullopt;

        case Target::presets:
            return editor.presetPrevButton.getScreenBounds()
                .getUnion(editor.presetNameButton.getScreenBounds())
                .getUnion(editor.presetNextButton.getScreenBounds());

        case Target::tape:
            if (spotlightDetail == SpotlightDetail::tapeDrive)
                return editor.tapeDriveSlider.getScreenBounds();
            if (spotlightDetail == SpotlightDetail::tapeFlavors || spotlightDetail == SpotlightDetail::tapeNoise)
                return editor.tapeFlavorBox.getScreenBounds();
            return editor.tapeLabel.getScreenBounds()
                .getUnion(editor.tapeFlavorBox.getScreenBounds())
                .getUnion(editor.tapeDriveSlider.getScreenBounds());

        case Target::delay:
        {
            using DelayRegion = DelayPanel::WalkthroughRegion;
            auto& delay = editor.delayPanel;
            if (spotlightDetail == SpotlightDetail::delayMode)
                return delay.getWalkthroughScreenBounds(DelayRegion::mode);
            if (spotlightDetail == SpotlightDetail::delayRate)
                return delay.getWalkthroughScreenBounds(DelayRegion::freeTime);
            if (spotlightDetail == SpotlightDetail::delaySync)
                return delay.getWalkthroughScreenBounds(DelayRegion::sync);
            if (spotlightDetail == SpotlightDetail::delayHeads)
                return delay.getWalkthroughScreenBounds(DelayRegion::heads);
            if (spotlightDetail == SpotlightDetail::delayFeedback)
                return delay.getWalkthroughScreenBounds(DelayRegion::feedback);
            if (spotlightDetail == SpotlightDetail::delayPingPong)
                return delay.getWalkthroughScreenBounds(DelayRegion::pingPong);
            if (spotlightDetail == SpotlightDetail::delayLevel)
                return delay.getWalkthroughScreenBounds(DelayRegion::echoLevel);
            if (spotlightDetail == SpotlightDetail::delayDub)
                return delay.getWalkthroughScreenBounds(DelayRegion::dub);
            return delay.getScreenBounds();
        }

        case Target::echoEngine:
            // Auto-Panic lives in the wordmark menu; point there until it opens.
            if (spotlightDetail == SpotlightDetail::echoAutoPanic)
                return editor.brandButton.getScreenBounds();
            return editor.echoEngineDropdown.getWalkthroughScreenBounds();

        case Target::reverb:
            if (spotlightDetail == SpotlightDetail::reverbOverview)
                return editor.reverbDropdown.getScreenBounds().getUnion(
                    editor.reverbPanel.getWalkthroughOverviewScreenBounds());
            if (spotlightDetail == SpotlightDetail::reverbSizeDecay)
                return editor.reverbPanel.getWalkthroughSizeDecayScreenBounds();
            if (spotlightDetail == SpotlightDetail::reverbTone)
                return editor.reverbPanel.getWalkthroughToneScreenBounds();
            if (spotlightDetail == SpotlightDetail::reverbLevel)
                return editor.reverbPanel.getWalkthroughLevelScreenBounds();
            return editor.reverbDropdown.getScreenBounds();

        case Target::reverbRouting:
            return editor.reverbPanel.getWalkthroughRoutingScreenBounds();

        case Target::spatialPosition:
            if (spotlightDetail == SpotlightDetail::spatialEnable)
                return spatial.getWalkthroughEnableScreenBounds();
            if (spotlightDetail == SpotlightDetail::spatialPreset)
                return spatial.getWalkthroughPresetScreenBounds();
            if (spotlightDetail == SpotlightDetail::spatialMode)
                return spatial.getWalkthroughModeScreenBounds();
            if (spotlightDetail == SpotlightDetail::spatialHeads)
                return spatial.getWalkthroughHeadSelectorScreenBounds();
            if (spotlightDetail == SpotlightDetail::spatialView ||
                spotlightDetail == SpotlightDetail::spatialDistanceView)
                return spatial.getWalkthroughHeadViewScreenBounds();
            if (spotlightDetail == SpotlightDetail::spatialReadout)
                return spatial.getWalkthroughPositionReadoutScreenBounds();
            return spatial.getWalkthroughPositionScreenBounds();

        case Target::spatialBlend:
            return spatial.getWalkthroughBlendScreenBounds();

        case Target::spatialMotion:
            if (spotlightDetail == SpotlightDetail::motionPatterns)
                return spatial.getWalkthroughMotionPatternScreenBounds();
            if (spotlightDetail == SpotlightDetail::motionRate)
                return spatial.getWalkthroughMotionRateScreenBounds().getUnion(
                    spatial.getWalkthroughMotionSyncScreenBounds());
            if (spotlightDetail == SpotlightDetail::motionDivision)
                return spatial.getWalkthroughMotionDivisionControlScreenBounds();
            if (spotlightDetail == SpotlightDetail::motionDepth)
                return spatial.getWalkthroughMotionDepthScreenBounds();
            if (spotlightDetail == SpotlightDetail::motionChoreo)
                return spatial.getWalkthroughMotionFineTuneButtonScreenBounds();
            return spatial.getWalkthroughMotionScreenBounds();

        case Target::mixOutput:
            return editor.mixField.getScreenBounds()
                .getUnion(editor.mixLockButton.getScreenBounds())
                .getUnion(editor.outField.getScreenBounds());

        case Target::utilities:
            return editor.brandButton.getScreenBounds();
    }

    return std::nullopt;
}

//==============================================================================
// Completion
//==============================================================================

std::map<juce::String, float> WalkthroughController::readTrackedValues() const
{
    std::map<juce::String, float> values;
    auto& vts = editor.audioProcessor.getValueTreeState();
    for (const auto* parameterId : kTrackedParameterIds)
        if (auto* raw = vts.getRawParameterValue(parameterId))
            values[parameterId] = raw->load();
    return values;
}

bool WalkthroughController::anyParameterChanged(std::initializer_list<const char*> parameterIds) const
{
    constexpr float kChangeThreshold = 1.0e-4f;
    auto& vts = editor.audioProcessor.getValueTreeState();
    for (const auto* parameterId : parameterIds)
    {
        const auto baseline = baselineValues.find(parameterId);
        auto* raw = vts.getRawParameterValue(parameterId);
        jassert(baseline != baselineValues.end() && raw != nullptr);  // every checked ID is tracked
        if (baseline == baselineValues.end() || raw == nullptr)
            continue;

        const float current = raw->load();
        if (std::isfinite(current) && std::isfinite(baseline->second) &&
            std::abs(current - baseline->second) > kChangeThreshold)
            return true;
    }
    return false;
}

bool WalkthroughController::isActionComplete(CompletionSignal completionSignal) const
{
    namespace motion = SpatialIDs::motion;
    namespace taps = SpatialIDs::taps;
    using Signal = CompletionSignal;

    switch (completionSignal)
    {
        case Signal::presetBrowser:
            return editor.presetBrowser.isVisible();
        case Signal::presetAudition:
            return static_cast<int>(editor.currentPresetType) != baselinePresetType ||
                   editor.currentPresetName != baselinePresetName;
        case Signal::presetFilter:
            return editor.presetBrowser.isVisible() && editor.presetBrowser.getWalkthroughFilterText().isNotEmpty() &&
                   editor.presetBrowser.getWalkthroughFilterText() != baselinePresetFilter;

        case Signal::tapeFlavor:
            return anyParameterChanged({"TapeFlavor"});
        case Signal::tapeDrive:
            return anyParameterChanged({"TapeDrive"});
        case Signal::tapeNoise:
            return anyParameterChanged({"TapeNoiseEnabled"});
        case Signal::delayMode:
            return anyParameterChanged({"DelaySetting"});
        case Signal::delayRate:
            return anyParameterChanged({"RepeatRate"});
        case Signal::delayFeedback:
            return anyParameterChanged({"Intensity"});
        case Signal::delayLevel:
            return anyParameterChanged({"EchoLevel"});
        case Signal::delayPingPong:
            return anyParameterChanged({"PingPong"});
        case Signal::delaySync:
            return anyParameterChanged({"TempoSync"});
        case Signal::delayHeads:
            return anyParameterChanged({"Head1NoteDivision", "Head2NoteDivision", "Head3NoteDivision"});
        case Signal::delayDub:
            return anyParameterChanged({"EchoSend"});
        case Signal::echoHeadRatio:
            return anyParameterChanged({"HeadRatioPreset"});
        case Signal::echoAutoPanic:
            return anyParameterChanged({"AutoPanic"});
        case Signal::reverbEngine:
            return anyParameterChanged({"ReverbEngine"});
        case Signal::reverbPreset:
            return anyParameterChanged({"ReverbSize", "ReverbDecay", "ReverbTone", "ReverbCharacter", "ReverbMod",
                                        "ReverbWidth", "ReverbPreDelay", "ReverbDuckAmount"});
        case Signal::reverbSizeDecay:
            return anyParameterChanged({"ReverbSize", "ReverbDecay"});
        case Signal::reverbTone:
            return anyParameterChanged({"ReverbTone"});
        case Signal::reverbCharacter:
            return anyParameterChanged({"ReverbCharacter"});
        case Signal::reverbMod:
            return anyParameterChanged({"ReverbMod"});
        case Signal::reverbPreDelayWidth:
            return anyParameterChanged({"ReverbWidth", "ReverbPreDelay"});
        case Signal::reverbDucking:
            return anyParameterChanged({"ReverbDuckAmount"});
        case Signal::reverbLevel:
            return anyParameterChanged({"ReverbLevel"});
        case Signal::reverbRouting:
            return anyParameterChanged({"ReverbRouting"});
        case Signal::spatialEnable:
            return anyParameterChanged({SpatialIDs::binaural::kEnabled});
        case Signal::spatialMode:
            return anyParameterChanged({"HeadPositionMode"});
        case Signal::spatialPosition:
            return anyParameterChanged({SpatialIDs::binaural::kAzimuth, SpatialIDs::binaural::kElevation,
                                        taps::kHead1Azimuth, taps::kHead1Elevation, taps::kHead2Azimuth,
                                        taps::kHead2Elevation, taps::kHead3Azimuth, taps::kHead3Elevation});
        case Signal::spatialDistance:
            return anyParameterChanged(
                {SpatialIDs::binaural::kDistance, taps::kHead1Distance, taps::kHead2Distance, taps::kHead3Distance});
        case Signal::spatialExactPosition:
            return anyParameterChanged({SpatialIDs::binaural::kAzimuth, SpatialIDs::binaural::kElevation,
                                        SpatialIDs::binaural::kDistance, taps::kHead1Azimuth, taps::kHead1Elevation,
                                        taps::kHead1Distance, taps::kHead2Azimuth, taps::kHead2Elevation,
                                        taps::kHead2Distance, taps::kHead3Azimuth, taps::kHead3Elevation,
                                        taps::kHead3Distance, SpatialIDs::binaural::kLockAzimuth,
                                        SpatialIDs::binaural::kLockElevation, SpatialIDs::binaural::kLockDistance});
        case Signal::spatialPreset:
            return anyParameterChanged(
                {SpatialIDs::binaural::kWidth, SpatialIDs::binaural::kCrossover, SpatialIDs::routing::kSpatialRouting,
                 motion::kPattern, motion::kRate, motion::kSync, motion::kDivision, motion::kLongDivision,
                 motion::kLongVariantDivision, motion::kDepth, motion::kRetrig, motion::kChoreo, motion::kSpread});
        case Signal::spatialChain:
            return anyParameterChanged({SpatialIDs::routing::kSpatialRouting});
        case Signal::spatialWet:
            return anyParameterChanged({SpatialIDs::binaural::kWetMix});
        case Signal::spatialWidth:
            return anyParameterChanged({SpatialIDs::binaural::kWidth});
        case Signal::spatialCrossover:
            return anyParameterChanged({SpatialIDs::binaural::kCrossover});
        case Signal::spatialQuality:
            return anyParameterChanged({SpatialIDs::binaural::kQuality});
        case Signal::motionPattern:
            return anyParameterChanged({motion::kPattern});
        case Signal::motionRate:
            return anyParameterChanged({motion::kRate, motion::kSync});
        case Signal::motionDivision:
            return anyParameterChanged(
                {motion::kSync, motion::kDivision, motion::kLongDivision, motion::kLongVariantDivision});
        case Signal::motionDepth:
            return anyParameterChanged({motion::kDepth});
        case Signal::motionRetrig:
            return anyParameterChanged({motion::kRetrig});
        case Signal::motionChoreo:
            return anyParameterChanged({motion::kChoreo});
        case Signal::motionSpread:
            return anyParameterChanged({motion::kSpread});
        case Signal::mixOutput:
            return anyParameterChanged({"Mix", "MixLock", "OutputGain", "EchoLevel", "ReverbLevel"});
        case Signal::utilities:
        case Signal::none:
            return false;
    }

    jassertfalse;
    return false;
}
