// SpatialEchoAudioProcessorEditor: Main plugin editor: header strip (wordmark menu, preset bar, OUT/MIX fields), ECHO
//   ENGINE section (delay, tape, reverb) and SPATIAL section (3D head view, motion), plus the help, preset-browser,
//   walkthrough and scope overlays. Binds the APVTS attachments for the controls it owns directly.

#pragma once

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <optional>
#include "PluginProcessor.h"
#include "Presets/FactoryPresetLibrary.h"
#include "UI/Components/ReverbPanel.h"
#include "UI/Components/EchoEngineDropdown.h"
#include "UI/Components/ReverbDropdown.h"
#include "UI/Components/SpatialPanel.h"
#include "UI/Components/DelayPanel.h"
#include "UI/Components/CRTLookAndFeel.h"
#include "UI/Components/ParameterValueSlider.h"
#include "UI/Components/ParameterValueComboBox.h"
#include "UI/Components/PhosphorScope.h"
#include "UI/Components/SectionWrapper.h"
#include "UI/Components/HeaderValueField.h"
#include "UI/Components/HelpOverlay.h"
#include "UI/Components/InteractiveWalkthrough.h"
#include "UI/Components/PresetBrowserOverlay.h"
#include "UI/Components/EditorPreferences.h"
#include "UI/WalkthroughController.h"

//==============================================================================
class SpatialEchoAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    SpatialEchoAudioProcessorEditor(SpatialEchoAudioProcessor&,
                                    std::unique_ptr<EditorPreferences> preferences = nullptr);
    ~SpatialEchoAudioProcessorEditor() override;

    //==============================================================================
    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void visibilityChanged() override;

    // Hosts call this for display scaling; the user's window zoom multiplies it.
    void setScaleFactor(float newScale) override;
    void setWindowZoom(float zoom);
    float getWindowZoom() const noexcept { return windowZoom; }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    void setValueDisclosuresForTesting(bool motionFineTune, bool blendExpanded)
    {
        spatialPanel.setValueDisclosuresForTesting(motionFineTune, blendExpanded);
        resized();
    }

    void showReverbDropdownForTesting() { reverbDropdown.showDropdown(); }

    juce::Component* getReverbDropdownWindowForTesting() const { return reverbDropdown.getDropdownWindowForTesting(); }

    void showEchoEngineDropdownForTesting() { echoEngineDropdown.showDropdown(); }

    juce::Component* getEchoEngineDropdownWindowForTesting() const
    {
        return echoEngineDropdown.getDropdownWindowForTesting();
    }

    bool applyReverbPresetSelectionForTesting(int engine, int presetIndex)
    {
        return reverbDropdown.applyPresetSelectionForTesting(engine, presetIndex);
    }

    void startWalkthroughForTesting() { walkthroughController.start(false); }
    void advanceWalkthroughForTesting()
    {
        interactiveWalkthrough.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    }
    void exitWalkthroughForTesting() { interactiveWalkthrough.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)); }
    int getWalkthroughStepCountForTesting() const { return interactiveWalkthrough.getStepCountForTesting(); }
    void setWalkthroughPresetIdentityForTesting(int selectionType, const juce::String& name)
    {
        currentPresetType = static_cast<PresetSelectionType>(juce::jlimit(0, 2, selectionType));
        currentPresetName = name;
    }
    void setWalkthroughPresetFilterForTesting(const juce::String& text, bool visible)
    {
        presetBrowser.setWalkthroughFilterTextForTesting(text);
        presetBrowser.setVisible(visible);
    }
    void prepareWalkthroughPresetCompletionForTesting()
    {
        walkthroughController.prepareStep(InteractiveWalkthrough::Target::presets,
                                          InteractiveWalkthrough::ExpandedTarget::none,
                                          InteractiveWalkthrough::SpotlightDetail::none);
    }
    void prepareWalkthroughCompletionForTesting(InteractiveWalkthrough::Target target)
    {
        walkthroughController.prepareStep(target, InteractiveWalkthrough::ExpandedTarget::none,
                                          InteractiveWalkthrough::SpotlightDetail::none);
    }
    bool isWalkthroughCompletionForTesting(InteractiveWalkthrough::CompletionSignal signal) const
    {
        return walkthroughController.isActionComplete(signal);
    }
    bool isWalkthroughPresetBrowserVisibleForTesting() const { return presetBrowser.isVisible(); }
    juce::Rectangle<int> getWalkthroughSpotlightForTesting() const
    {
        return interactiveWalkthrough.getSpotlightBoundsForTesting();
    }
    std::optional<juce::Rectangle<int>> getResolvedWalkthroughDelayRateTargetForTesting() const
    {
        return walkthroughController.resolveTarget(InteractiveWalkthrough::Target::delay,
                                                   InteractiveWalkthrough::ExpandedTarget::none,
                                                   InteractiveWalkthrough::SpotlightDetail::delayRate);
    }

#endif

private:
    // The tour drives the editor's own surfaces and reads its controls.
    friend class WalkthroughController;

    SpatialEchoAudioProcessor& audioProcessor;
    CRTLookAndFeel crtLookAndFeel;
    PhosphorScope phosphorScope{audioProcessor.getScopeDataFifo()};
    juce::Rectangle<int> headerTitleBounds;
    bool headerCompactWordmark = false;  // narrow widths: "SE" so MIX/OUT stay visible
    bool scopeVisible = false;

    // Wordmark = clickable app-menu trigger. An invisible button over the
    // painted wordmark provides hit-testing + the pointing cursor; paint()
    // brightens the phosphor on hover / while the menu is open, and a one-shot
    // shimmer plays each time the editor opens to teach the click.
    float headerWordmarkFontHeight = 28.0f;  // set in resized(), mirrored in paint()
    bool wordmarkHovered = false;
    bool wordmarkMenuOpen = false;
    bool wordmarkShimmerActive = false;
    float wordmarkShimmerPhase = 0.0f;  // 0..1, advanced by the 30 Hz timer
    struct InvisibleButton : public juce::Button
    {
        InvisibleButton()
            : juce::Button("brand")
        {
        }
        void paintButton(juce::Graphics&, bool, bool) override {}
    };
    InvisibleButton brandButton;

    // ------ GUI Components ------

    // Master strip (header): Mix + Output value fields with a Mix padlock.
    struct LockGlyphLookAndFeel : public juce::LookAndFeel_V4
    {
        void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    };
    // DUB is a lamp in DelayPanel; AUTO-PANIC is a wordmark-menu item.
    HeaderValueField outField;
    HeaderValueField mixField;
    juce::ToggleButton mixLockButton;
    LockGlyphLookAndFeel lockGlyphLookAndFeel;
    std::atomic<float>* mixLockValue = nullptr;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mixLockAttachment;

    DelayPanel delayPanel;
    // Tape surface: flavor selector with a noise override + DRIVE, delay panel
    // top-left. The popup hook preserves the closed combo's value layout.
    class TapeFlavorComboBox final : public ParameterValueComboBox
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
    TapeFlavorComboBox tapeFlavorBox;
    // TAPE group: frameless column - TAPE caption above the
    // flavor combo, unlabelled DRIVE slider below (value text + tooltip).
    juce::Label tapeLabel;
    ParameterValueSlider tapeDriveSlider;

    void showTapeFlavorMenu();
    void completeTapeFlavorMenu(int generation);
    void
    showHeadSubdivisionMenu(int headNumber,
                            bool forWalkthrough = false);  // Shows per-head subdivision selector (1=H1, 2=H2, 3=H3)
    void dismissDivisionMenu();                            // Closes the subdivision menu (select/outside-click/ESC)
    std::unique_ptr<juce::Component> divisionMenuWindow;   // Host window for the open subdivision menu

    // Preset UI
    juce::TextButton presetPrevButton;
    juce::TextButton presetNextButton;
    juce::TextButton presetNameButton;
    PresetBrowserOverlay presetBrowser;  // auditionable browser + preset manager
    void showPresetBrowser();
    // Preset actions (shared by the browser's action bar / inline affordances)
    void presetSave();
    void presetSaveAs();
    void presetImport();
    void presetExport();
    void presetRevealFolder();
    void presetRename(const juce::String& existingName);
    void presetDelete(const juce::String& presetName);
    enum class PresetSelectionType
    {
        Init,
        Factory,
        User
    };
    PresetSelectionType currentPresetType = PresetSelectionType::Init;
    juce::String currentPresetName;
    juce::StringArray cachedFactoryPresetNames;
    juce::StringArray cachedPresetNames;
    std::unique_ptr<juce::FileChooser> presetFileChooser;
    juce::File presetLastDirectory;

    // Dirty-state indicator: compare only musical preset parameters.
    std::array<float, FactoryPresetLibrary::kParameterIds.size()> presetStateSnapshotValues{};
    std::array<float, FactoryPresetLibrary::kParameterIds.size()> presetStateSnapshotEpsilons{};
    std::array<bool, FactoryPresetLibrary::kParameterIds.size()> presetStateSnapshotDiscrete{};
    std::array<bool, FactoryPresetLibrary::kParameterIds.size()> presetStateSnapshotTracked{};
    bool presetStateSnapshotValid = false;
    bool presetDirty = false;
    int dirtyCheckCounter = 0;
    void capturePresetSnapshot();
    bool isCurrentStateDirtyAgainstSnapshot() const;
    static bool shouldUseDiscretePresetComparison(const juce::RangedAudioParameter& parameter);
    static float getPresetContinuousComparisonEpsilon(const juce::RangedAudioParameter& parameter);

    // Section headers that open their drawers.
    EchoEngineDropdown echoEngineDropdown;  // Head ratio + engine status light
    ReverbDropdown reverbDropdown;          // Clickable "REVERB" -> Reverb fine-tune

    ReverbPanel reverbPanel;  // Size, Decay, Tone, Routing, Level

    // Decorative borders around the two content sections.
    SectionWrapper echoEngineSection;
    SectionWrapper spatialSection;

    // SPATIAL section: head view, position, blend drawer and motion.
    SpatialPanel spatialPanel;

    // ------ Attachments (connect GUI to parameters) ------

    // DUB attaches in DelayPanel; AUTO-PANIC is set from showAppMenu().
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> tapeFlavorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tapeDriveAttachment;

    // Wow/flutter has no attachment: it is a per-flavor internal value.
    // Reverb attachments live in ReverbPanel and ReverbDropdown.

    // Tooltip window with enable/disable support
    struct ToggleableTooltip : public juce::TooltipWindow
    {
        ToggleableTooltip(Component* p, int ms)
            : juce::TooltipWindow(p, ms)
        {
        }
        bool enabled = true;
        juce::String getTipFor(Component& c) override
        {
            if (!enabled)
                return {};

            return juce::TooltipWindow::getTipFor(c);
        }
    };
    // Desktop-parented (nullptr): an editor-child tooltip is composited under
    // the GL head view. LookAndFeel is set explicitly in the constructor - a
    // desktop window inherits none, and the default LnF is standalone-only.
    ToggleableTooltip tooltipWindow{nullptr, 500};

    // Help overlay - toggleable shortcut/hint panel ('?' key or the utility menu)
    HelpOverlay helpOverlay;

    // Per-user editor preferences (tour seen, window zoom, per-head hint).
    // Deliberately separate from APVTS, presets, and host project state.
    std::unique_ptr<EditorPreferences> editorPreferences;
    float hostScaleFactor = 1.0f;
    float windowZoom = 1.0f;

    // First-run feature tour overlay; WalkthroughController drives it.
    InteractiveWalkthrough interactiveWalkthrough;

    // About dialog tracking for GL z-order fix (detach GL while dialog is open)
    juce::Component::SafePointer<juce::DialogWindow> aboutDialog;
    bool lastAboutDialogState = false;  // Track state change to reattach GL when closed

    // Cached background image for scanlines + gradient (rebuilt on resize only)
    juce::Image cachedBackground;

    // Undo transaction coalescing (group rapid changes; split on 300ms pause)
    juce::uint32 lastUndoActivityTime = 0;

    // Head view sync (keeps the view aligned with automation and MIDI)
    float lastSpatialAzimuth = 0.0f;
    float lastSpatialElevation = 0.0f;
    float lastSpatialDistance = 1.0f;
    bool lastSpatialEnabled = false;

    void refreshPresetList();
    void updatePresetDisplay();
    void selectRelativePreset(int delta);
    void loadPresetSelection(PresetSelectionType type, const juce::String& name, const juce::String& transactionName);
    void showAppMenu();  // wordmark -> app / utility menu (settings, help, about)
    EditorPreferences& ensureEditorPreferences();
    // The preferences to read and persist, or nullptr for an editor built
    // without a host wrapper or injected preferences (headless tests), which
    // must not touch the user's settings file.
    EditorPreferences* storedPreferences();
    void showAboutDialog();
    void updateHeadViewGLForOverlays();  // z-order: detach GL when overlays visible
    void confirmPresetAction(const juce::String& title, const juce::String& message, const juce::String& actionLabel,
                             std::function<void(bool)> onResult);
    void showPresetMessage(const juce::String& title, const juce::String& message);
    void promptForPresetName(const juce::String& title, const juce::String& initialName,
                             const juce::String& actionLabel, std::function<void(const juce::String&)> onConfirm,
                             bool allowEmpty = false, const juce::String& fieldLabel = "Name:");

    // Declared last: destroyed first, while every surface it reaches exists.
    WalkthroughController walkthroughController{*this};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialEchoAudioProcessorEditor)
};
