// SpatialEchoAudioProcessorEditor: Main plugin editor implementation. A header strip (wordmark menu, preset bar,
//   OUT/MIX fields) sits above two sections: ECHO ENGINE (delay heads, tape flavor + drive, reverb) on the left and
//   SPATIAL (3D head view, motion) on the right, with help, preset-browser, walkthrough and scope overlays.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets/FactoryPresetLibrary.h"
#include "Presets/FactoryPresetDescriptions.h"
#include "Presets/FactoryPresetBrowseGroups.h"
#include "Presets/ReverbPresetLibrary.h"
#include "Presets/ReverbPresetApplicator.h"
#include "ParameterWriteHelpers.h"
#include "SpatialParamIDs.h"
#include "UI/UIConstants.h"
#include "UI/Components/MenuChrome.h"
#include "UI/Components/DivisionMenuGrid.h"
#include "UI/Components/DropdownWindowUtils.h"
#include <array>
#include <cmath>

namespace
{
constexpr const char* kPresetFileExtension = "spatialEchopreset";

CRTLookAndFeel& getSharedPopupLookAndFeel()
{
    static CRTLookAndFeel sharedLookAndFeel;
    return sharedLookAndFeel;
}

int& getActiveEditorCount()
{
    static int activeEditorCount = 0;
    return activeEditorCount;
}

void notifyHostOfPresetParameterValues(juce::AudioProcessorValueTreeState& valueTreeState)
{
    for (const auto* parameterId : FactoryPresetLibrary::kParameterIds)
    {
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(valueTreeState.getParameter(parameterId)))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost(parameter->getValue());
            parameter->endChangeGesture();
        }
    }
}

juce::String sanitizePresetNameForUi(juce::String name)
{
    name = name.trim();
    if (name.isEmpty())
        return {};
    name = name.replaceCharacters("\\/:*?\"<>|", "_________");
    while (name.contains("  "))
        name = name.replace("  ", " ");
    return name.trim();
}

// Wordmark dropdown caret, proportional to the wordmark font height.
constexpr float kWordmarkCaretWidthRatio = 0.42f;
constexpr float kWordmarkCaretGapRatio = 0.30f;

// "PRESET" legend etched into the preset bar frame.
constexpr float kPresetLegendFontSize = 9.0f;

float getPresetPlainValue(const juce::RangedAudioParameter& parameter, bool discrete)
{
    float plainValue = parameter.convertFrom0to1(parameter.getValue());
    if (discrete)
        plainValue = static_cast<float>(juce::roundToInt(plainValue));
    return plainValue;
}

std::unique_ptr<juce::Drawable> loadSealDrawable()
{
    auto svgXml = juce::XmlDocument::parse(
        juce::String::fromUTF8(BinaryData::echoeslabsealgreen_svg, BinaryData::echoeslabsealgreen_svgSize));
    if (svgXml == nullptr)
        return {};

    return juce::Drawable::createFromSVG(*svgXml);
}

constexpr float kAboutTitleFontSize = 18.0f;

class AboutDialogContent final : public juce::Component
{
public:
    AboutDialogContent()
    {
        setSize(520, 600);

        closeButton.setButtonText("CLOSE");
        closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        closeButton.setColour(juce::TextButton::buttonOnColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.12f));
        closeButton.setColour(juce::TextButton::textColourOffId, CRTLookAndFeel::getTerminalGreen());
        closeButton.setColour(juce::TextButton::textColourOnId, CRTLookAndFeel::getTerminalGreen());
        closeButton.onClick = [this]()
        {
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
                w->exitModalState(0);
        };
        addAndMakeVisible(closeButton);

        openLabButton.setButtonText("OPEN LAB");
        openLabButton.setColour(juce::TextButton::buttonColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.10f));
        openLabButton.setColour(juce::TextButton::buttonOnColourId,
                                CRTLookAndFeel::getTerminalGreen().withAlpha(0.16f));
        openLabButton.setColour(juce::TextButton::textColourOffId, CRTLookAndFeel::getTerminalGreen());
        openLabButton.setColour(juce::TextButton::textColourOnId, CRTLookAndFeel::getTerminalGreen());
        openLabButton.onClick = [] { juce::URL("https://echoeslabmusic.com/lab").launchInDefaultBrowser(); };
        addAndMakeVisible(openLabButton);

        sealDrawable = loadSealDrawable();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        auto buttonRow = area.removeFromBottom(38);
        const int buttonGap = 8;
        const int closeW = 98;
        const int openW = 120;
        closeButton.setBounds(buttonRow.removeFromRight(closeW));
        buttonRow.removeFromRight(buttonGap);
        openLabButton.setBounds(buttonRow.removeFromRight(openW));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(CRTLookAndFeel::getVoidBlack());
        CRTLookAndFeel::drawGradientOverlay(g, getLocalBounds());
        CRTLookAndFeel::drawScanlines(g, getLocalBounds());

        auto panel = getLocalBounds().reduced(12).toFloat();
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.05f));
        g.fillRoundedRectangle(panel, 10.0f);
        CRTLookAndFeel::drawTerminalBorder(g, panel, 10.0f);

        auto content = panel.toNearestInt().reduced(18);

        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.88f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(kAboutTitleFontSize));
        g.drawText("SPATIAL ECHO", content.removeFromTop(36), juce::Justification::centred);

        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.52f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
        g.drawText("ABOUT", content.removeFromTop(20), juce::Justification::centred);

        content.removeFromTop(6);

        auto sealArea = content.removeFromTop(250).toFloat().reduced(20.0f, 8.0f);
        if (sealDrawable != nullptr)
        {
            sealDrawable->drawWithin(g, sealArea, juce::RectanglePlacement::centred, 1.0f);
        }
        else
        {
            g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.35f));
            g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
            g.drawFittedText("SEAL NOT FOUND", sealArea.toNearestInt(), juce::Justification::centred, 1);
        }

        content.removeFromTop(8);

        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.75f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeBodySize));
        g.drawFittedText("Version " JucePlugin_VersionString, content.removeFromTop(24), juce::Justification::centred,
                         1);

        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.55f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
        g.drawFittedText("Tape echo, reverb, and spatial processing", content.removeFromTop(20),
                         juce::Justification::centred, 1);

        g.drawFittedText("echoeslabmusic.com/lab", content.removeFromTop(22), juce::Justification::centred, 1);

        // Steinberg's VST trademark terms ask for this notice in the product credits.
        content.removeFromTop(14);
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.40f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
        g.drawFittedText("VST is a registered trademark of", content.removeFromTop(16), juce::Justification::centred,
                         1);
        g.drawFittedText("Steinberg Media Technologies GmbH.", content.removeFromTop(16), juce::Justification::centred,
                         1);
    }

private:
    juce::TextButton closeButton;
    juce::TextButton openLabButton;
    std::unique_ptr<juce::Drawable> sealDrawable;
};
}  // namespace

//==============================================================================
// Header Mix lock: shared CRT padlock glyph (same treatment as the spatial
// panel's per-axis locks).
void SpatialEchoAudioProcessorEditor::LockGlyphLookAndFeel::drawToggleButton(juce::Graphics& g,
                                                                             juce::ToggleButton& button,
                                                                             bool shouldDrawButtonAsHighlighted,
                                                                             bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(shouldDrawButtonAsDown);
    CRTLookAndFeel::drawPadlockGlyph(g, button.getLocalBounds().toFloat(), button.getToggleState(),
                                     shouldDrawButtonAsHighlighted);
}

//==============================================================================
SpatialEchoAudioProcessorEditor::SpatialEchoAudioProcessorEditor(SpatialEchoAudioProcessor& p,
                                                                 std::unique_ptr<EditorPreferences> preferences)
    : AudioProcessorEditor(&p)
    , audioProcessor(p)
    , outField(*p.getValueTreeState().getParameter("OutputGain"), "OUT", 70)
    , mixField(*p.getValueTreeState().getParameter("Mix"), "MIX", 58)
    , delayPanel(p.getValueTreeState())
    , echoEngineDropdown(p, p.getValueTreeState())
    , reverbDropdown(p.getValueTreeState())
    , reverbPanel(p.getValueTreeState())
    , spatialPanel(p.getValueTreeState())
    , editorPreferences(std::move(preferences))
{
    using namespace UIConstants;
    setSize(editorWidth, editorHeight);
    setResizable(true, true);
    // The minimum size keeps every control at the 24px minimum hit target after scaling.
    setResizeLimits(editorWidth, editorHeight, editorMaxWidth, editorMaxHeight);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(editorWidth) / editorHeight);  // no layout distortion

    // Window Size preference: 75% fits 1366-wide screens and 150%-scaled 1080p
    // Windows laptops. Headless construction without preferences stays at 100%.
    if (auto* prefs = storedPreferences())
        windowZoom = prefs->getWindowZoom();
    if (!juce::exactlyEqual(windowZoom, 1.0f))
        AudioProcessorEditor::setScaleFactor(hostScaleFactor * windowZoom);

    ++getActiveEditorCount();

    // Apply the CRT LookAndFeel to wrapper chrome in standalone mode.
    // Only set default LookAndFeel in standalone (isStandaloneApp) to avoid bleeding
    // into host-owned UI in JUCE-based hosts.
    // Reachable: Options button, DocumentWindow title bar, popup menus
    // Unreachable (hardcoded in JUCE): NotificationArea (mute feedback banner)
    if (juce::JUCEApplicationBase::isStandaloneApp())
        juce::LookAndFeel::setDefaultLookAndFeel(&getSharedPopupLookAndFeel());
    setLookAndFeel(&crtLookAndFeel);
    tooltipWindow.setLookAndFeel(&getSharedPopupLookAndFeel());
    presetBrowser.setLookAndFeel(&getSharedPopupLookAndFeel());

    auto& vts = audioProcessor.getValueTreeState();

    // Header master fields (OUT + Mix). Typed text goes through each
    // parameter's own text conversion ("-3 dB" / "-3", "50%" / "50").
    outField.setTooltip("Output gain (peak headroom dB). Drag vertically, double-click to type.");
    addAndMakeVisible(outField);

    mixField.setTooltip("Dry/wet mix (0%=dry, 100%=wet). Drag vertically, double-click to type.");
    addAndMakeVisible(mixField);

    mixLockButton.setTooltip("Lock the current Mix value. Useful for send/aux routing where Mix should stay fixed.");
    mixLockButton.setTitle("Mix Lock");
    mixLockButton.setLookAndFeel(&lockGlyphLookAndFeel);
    mixLockButton.setWantsKeyboardFocus(true);
    addAndMakeVisible(mixLockButton);
    mixLockAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(vts, "MixLock", mixLockButton);
    mixLockValue = vts.getRawParameterValue("MixLock");

    // ------ Section Wrappers (decorative borders - sent to back so they don't block) ------
    addAndMakeVisible(echoEngineSection);
    addAndMakeVisible(spatialSection);
    echoEngineSection.toBack();
    spatialSection.toBack();

    // ------ Delay Section ------
    addAndMakeVisible(delayPanel);
    delayPanel.onHeadClicked = [this](int headNumber) { showHeadSubdivisionMenu(headNumber); };

    // Tape flavor + DRIVE: the tape surface, delay panel top-left.
    if (auto* flavorParam = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter("TapeFlavor")))
        for (int i = 0; i < flavorParam->choices.size(); ++i)
            tapeFlavorBox.addItem(flavorParam->choices[i], i + 1);
    tapeFlavorBox.onPopupRequested = [this] { showTapeFlavorMenu(); };
    tapeFlavorBox.setTooltip("Tape flavor. The menu also lets you turn modeled tape noise on or off.");
    addAndMakeVisible(tapeFlavorBox);
    // TAPE group labels + DRIVE slider (drawer-row idiom: no etched frame,
    // no knob).
    auto styleTapeLabel = [this](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
        label.setColour(juce::Label::textColourId,
                        CRTLookAndFeel::getTerminalGreen().withAlpha(UIConstants::alphaLabel));
        label.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(label);
    };
    styleTapeLabel(tapeLabel, "TAPE");
    tapeLabel.setJustificationType(juce::Justification::centred);

    tapeDriveSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tapeDriveSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 22);
    // Transparent value box: the opaque default reads as a stray black
    // rectangle on the textured panel (the drawer sliders sit on plain black
    // so it never shows there).
    tapeDriveSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    tapeDriveSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    tapeDriveSlider.setTooltip("Drive. Pushes level into the tape: more saturation and earlier breakup. Centre "
                               "reproduces the flavor as tuned.");
    addAndMakeVisible(tapeDriveSlider);

    // The attachment also sets the slider's double-click reset to the
    // parameter default.
    tapeFlavorAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(vts, "TapeFlavor", tapeFlavorBox);
    tapeDriveAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(vts, "TapeDrive", tapeDriveSlider);

    // Phosphor oscilloscope (header area - behind all controls)
    addAndMakeVisible(phosphorScope);
    phosphorScope.setSampleRate(audioProcessor.getSampleRate());
    phosphorScope.toBack();

    // Scope visibility (persisted via ValueTree property); toggled from the
    // wordmark menu.
    scopeVisible = audioProcessor.getValueTreeState().state.getProperty("scopeVisible", false);
    phosphorScope.setVisible(scopeVisible);

    // Start timer to sync parameter-driven display states (no full-editor repaint)
    startTimerHz(30);  // 30 Hz for parameter sync only

    // ------ Reverb Section ------
    addAndMakeVisible(reverbPanel);

    // ------ Preset bar ------

    auto setupPresetButton = [this](juce::TextButton& button, const juce::String& text, const juce::String& tooltip)
    {
        button.setButtonText(text);
        button.setTooltip(tooltip);
        button.setColour(juce::TextButton::buttonColourId, CRTLookAndFeel::getVoidBlack().withAlpha(0.25f));
        button.setColour(juce::TextButton::buttonOnColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.2f));
        button.setColour(juce::TextButton::textColourOffId, CRTLookAndFeel::getTerminalGreen());
        button.setColour(juce::TextButton::textColourOnId, CRTLookAndFeel::getTerminalGreen());
        addAndMakeVisible(button);
    };

    setupPresetButton(presetPrevButton, "<", "Previous preset");
    setupPresetButton(presetNextButton, ">", "Next preset");
    setupPresetButton(presetNameButton, "Init", "Select preset");

    presetPrevButton.onClick = [this]() { selectRelativePreset(-1); };
    presetNextButton.onClick = [this]() { selectRelativePreset(1); };
    presetNameButton.onClick = [this]() { showPresetBrowser(); };

    // Wordmark = app-menu trigger. An invisible button over the painted
    // "SPATIAL ECHO" provides hit-testing, the pointing cursor, and keyboard
    // access; paint() supplies the hover/shimmer phosphor treatment.
    brandButton.setName("brandButton");
    brandButton.setTitle("Spatial Echo Menu");
    brandButton.setDescription("Open app settings, help, and about");
    brandButton.setTooltip("Settings, help, about");
    brandButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    brandButton.setWantsKeyboardFocus(true);
    brandButton.onClick = [this]() { showAppMenu(); };
    addAndMakeVisible(brandButton);

    // One-shot brand shimmer teaches the click each time the editor opens.
    wordmarkShimmerActive = true;
    wordmarkShimmerPhase = 0.0f;

    // Restore active preset identity from ValueTree (persisted by
    // updatePresetDisplay → getStateInformation / setStateInformation).
    {
        const auto& state = vts.state;
        currentPresetName = state.getProperty("activePresetName", juce::String()).toString();
        const int typeInt = static_cast<int>(state.getProperty("activePresetType", 0));
        currentPresetType = static_cast<PresetSelectionType>(juce::jlimit(0, 2, typeInt));
    }

    refreshPresetList();
    capturePresetSnapshot();

    // ------ Spatial Section ------
    addAndMakeVisible(spatialPanel);

    // The per-head hint stays dismissed once the user has tried per-head mode.
    if (auto* prefs = storedPreferences())
    {
        if (prefs->isPerHeadHintDismissed())
            spatialPanel.setPerHeadHintDismissed(true);
        else if (spatialPanel.isPerHeadHintDismissed())
            (void)prefs->markPerHeadHintDismissed();  // the session opened in per-head mode
    }
    spatialPanel.onPerHeadHintDismissed = [this]
    {
        if (auto* prefs = storedPreferences())
            (void)prefs->markPerHeadHintDismissed();
    };

    // Wire head-view callbacks to the parameter system. Gestures are
    // target-aware: the Linked path brackets the Spatial* params, the
    // per-tap path brackets the Head{n}* params of every tap being edited.
    auto& headView = spatialPanel.getHeadView();

    // Audio-reactive glow: the head view breathes with the echo-branch meter
    // the DSP publishes (read-only atomic, no FIFO contention).
    headView.audioLevelProvider = [this] { return audioProcessor.getEchoBranchMeterLevel(); };

    // Per-tap audio pulse: each marker's glow follows its tap's envelope level
    // (0 for an out-of-range tap). Separate from distance encoding so audio
    // never fakes distance.
    headView.tapAudioLevelProvider = [this](int tapIndex) -> float
    { return audioProcessor.getTapEnvelopeLevel(tapIndex); };

    auto forEachGestureParam =
        [this](SpatialHeadView::GestureMask mask, const std::function<void(const juce::String&)>& fn)
    {
        auto& view = spatialPanel.getHeadView();
        juce::StringArray prefixes;
        if (!view.isPerTapMode())
        {
            prefixes.add("Spatial");
        }
        else if (view.getEditTarget() == SpatialHeadView::Target::All)
        {
            prefixes.add("Head1");
            prefixes.add("Head2");
            prefixes.add("Head3");
        }
        else
        {
            prefixes.add("Head" + juce::String(view.primarySlotForTarget()));
        }

        for (const auto& prefix : prefixes)
        {
            if ((mask & SpatialHeadView::gestureAzimuth) != 0u)
                fn(prefix + "Azimuth");
            if ((mask & SpatialHeadView::gestureElevation) != 0u)
                fn(prefix + "Elevation");
            if ((mask & SpatialHeadView::gestureDistance) != 0u)
                fn(prefix + "Distance");
        }
    };

    headView.onGestureBegin = [this, forEachGestureParam](SpatialHeadView::GestureMask mask)
    {
        auto& treeState = audioProcessor.getValueTreeState();
        audioProcessor.noteSpatialManualInteraction();
        forEachGestureParam(mask, [&treeState](const juce::String& id)
                            { SpatialEchoParameterWrite::beginGestureById(treeState, id); });
    };

    headView.onGestureEnd = [this, forEachGestureParam](SpatialHeadView::GestureMask mask)
    {
        auto& treeState = audioProcessor.getValueTreeState();
        forEachGestureParam(mask, [&treeState](const juce::String& id)
                            { SpatialEchoParameterWrite::endGestureById(treeState, id); });
    };

    headView.performPositionBatch = [this](const std::function<void()>& edit)
    {
        SpatialEchoAudioProcessor::ScopedSpatialParameterBatch batch(audioProcessor);
        edit();
    };
    headView.onTapPositionChanged = [this](int tapIndex, float az, float el, float dist)
    {
        SpatialEchoAudioProcessor::ScopedSpatialParameterBatch batch(audioProcessor);
        auto& treeState = audioProcessor.getValueTreeState();
        audioProcessor.noteSpatialManualInteraction();
        const juce::String prefix = "Head" + juce::String(tapIndex + 1);
        SpatialEchoParameterWrite::setFloatById(treeState, prefix + "Azimuth", az);
        SpatialEchoParameterWrite::setFloatById(treeState, prefix + "Elevation", el);
        SpatialEchoParameterWrite::setFloatById(treeState, prefix + "Distance", dist);
    };

    headView.onAzimuthChanged = [this](float azimuth)
    {
        audioProcessor.noteSpatialManualInteraction();
        SpatialEchoParameterWrite::setFloatById(audioProcessor.getValueTreeState(), SpatialIDs::binaural::kAzimuth,
                                                azimuth);
    };

    headView.onDistanceChanged = [this](float distance)
    {
        audioProcessor.noteSpatialManualInteraction();
        SpatialEchoParameterWrite::setFloatById(audioProcessor.getValueTreeState(), SpatialIDs::binaural::kDistance,
                                                distance);
    };

    headView.onElevationChanged = [this](float elevation)
    {
        audioProcessor.noteSpatialManualInteraction();
        SpatialEchoParameterWrite::setFloatById(audioProcessor.getValueTreeState(), SpatialIDs::binaural::kElevation,
                                                elevation);
    };

    // Initialize the view from the current parameter values; the timer keeps
    // it in sync afterwards.
    const float initAz = vts.getRawParameterValue(SpatialIDs::binaural::kAzimuth)->load();
    const float initEl = vts.getRawParameterValue(SpatialIDs::binaural::kElevation)->load();
    const float initDist = vts.getRawParameterValue(SpatialIDs::binaural::kDistance)->load();
    headView.setAzimuth(initAz);
    headView.setElevation(initEl);
    headView.setDistance(initDist);
    lastSpatialAzimuth = initAz;
    lastSpatialElevation = initEl;
    lastSpatialDistance = initDist;
    if (auto* enableParam = vts.getRawParameterValue(SpatialIDs::binaural::kEnabled))
        lastSpatialEnabled = (*enableParam > 0.5f);

    addAndMakeVisible(echoEngineDropdown);
    addAndMakeVisible(reverbDropdown);

    // Help overlay - toggleable shortcut/hint panel ('?' key or the wordmark
    // menu's Help / Shortcuts entry).
    addChildComponent(helpOverlay);

    // Preset browser overlay (presets-first on-ramp): opened by the header
    // preset-name button; sits above everything while auditioning.
    addChildComponent(presetBrowser);

    // First-run walkthrough sits above editor-owned overlays. Its spotlight
    // passes input only to the feature currently being taught.
    addChildComponent(interactiveWalkthrough);

    // ------ Component names (accessibility and test lookup) ------
    // Section wrappers
    echoEngineSection.setName("echoEngineSection");
    spatialSection.setName("spatialSection");

    // Main panels
    delayPanel.setName("delayPanel");
    reverbPanel.setName("reverbPanel");
    spatialPanel.setName("spatialPanel");
    mixField.setName("mixField");
    mixLockButton.setName("mixLockButton");

    // Section labels
    echoEngineDropdown.setName("echoEngineDropdown");
    reverbDropdown.setName("reverbDropdown");

    // Header controls
    presetPrevButton.setName("presetPrevButton");
    presetNextButton.setName("presetNextButton");
    presetNameButton.setName("presetNameButton");

    // Echo section buttons
    tapeFlavorBox.setName("tapeFlavorBox");
    tapeDriveSlider.setName("tapeDriveSlider");
    tapeLabel.setName("tapeLabel");

    // ------ Keyboard Focus (Accessibility: WCAG 2.1.1) ------
    tapeFlavorBox.setWantsKeyboardFocus(true);
    tapeDriveSlider.setWantsKeyboardFocus(true);
    presetPrevButton.setWantsKeyboardFocus(true);
    presetNextButton.setWantsKeyboardFocus(true);
    presetNameButton.setWantsKeyboardFocus(true);

    // Explicit focus order for header/top-level controls
    presetPrevButton.setExplicitFocusOrder(2);
    presetNameButton.setExplicitFocusOrder(3);
    presetNextButton.setExplicitFocusOrder(4);
    tapeFlavorBox.setExplicitFocusOrder(6);
    tapeDriveSlider.setExplicitFocusOrder(7);

    // ------ Accessible Metadata (Accessibility: WCAG 4.1.2) ------
    // Header controls
    presetPrevButton.setTitle("Previous Preset");
    presetPrevButton.setDescription("Navigate to the previous preset in the list");
    presetNextButton.setTitle("Next Preset");
    presetNextButton.setDescription("Navigate to the next preset in the list");
    presetNameButton.setTitle("Preset Name");
    presetNameButton.setDescription("Click to open the preset browser");

    tapeFlavorBox.setTitle("Tape Flavor");
    tapeFlavorBox.setDescription("Selects the tape character and lets you turn modeled tape noise on or off");
    tapeDriveSlider.setTitle("Tape Drive");
    tapeDriveSlider.setDescription("Controls tape coloration, saturation, and breakup.");

    // Dropdown headers
    echoEngineDropdown.setTitle("Echo Engine Settings");
    echoEngineDropdown.setDescription("Click to expand echo engine head ratio settings");
    reverbDropdown.setTitle("Reverb Settings");
    reverbDropdown.setDescription("Click to expand reverb fine-tune settings");

    // GL z-order: the head view detaches its GL context while an overlay
    // covers it.
    headView.setOverlayReferences(&helpOverlay, &interactiveWalkthrough);

    // Help can now dismiss itself (ESC), which bypasses the toggle sites -
    // reattach GL through the same choke point on any visibility change.
    helpOverlay.onVisibilityChanged = [this] { updateHeadViewGLForOverlays(); };
}

SpatialEchoAudioProcessorEditor::~SpatialEchoAudioProcessorEditor()
{
    walkthroughController.detach();
    stopTimer();
    spatialPanel.onPerHeadHintDismissed = nullptr;
    spatialPanel.getHeadView().setOverlayReferences(nullptr, nullptr);
    tapeFlavorBox.onPopupRequested = nullptr;
    tooltipWindow.setLookAndFeel(nullptr);
    mixLockButton.setLookAndFeel(nullptr);
    echoEngineDropdown.hideDropdown();
    reverbDropdown.hideDropdown();
    presetFileChooser.reset();
    auto& activeEditorCount = getActiveEditorCount();
    if (activeEditorCount > 0)
        --activeEditorCount;

    if (activeEditorCount == 0 && &juce::LookAndFeel::getDefaultLookAndFeel() == &getSharedPopupLookAndFeel())
    {
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }
    setLookAndFeel(nullptr);
}

void SpatialEchoAudioProcessorEditor::timerCallback()
{
    // Wordmark: track hover and advance the one-shot open shimmer so paint()
    // can light the phosphor. Repaint only the wordmark region when it changes.
    {
        bool needsRepaint = false;
        const bool over = brandButton.isMouseOver(true);
        if (over != wordmarkHovered)
        {
            wordmarkHovered = over;
            needsRepaint = true;
        }
        if (wordmarkShimmerActive)
        {
            wordmarkShimmerPhase += 1.0f / 33.0f;  // ~1.1 s pulse at 30 Hz
            if (wordmarkShimmerPhase >= 1.0f)
            {
                wordmarkShimmerPhase = 1.0f;
                wordmarkShimmerActive = false;
            }
            needsRepaint = true;
        }
        if (needsRepaint)
            repaint(headerTitleBounds.expanded(4));
    }

    // Update enabled/disabled states for controls based on parameters
    auto& vts = audioProcessor.getValueTreeState();
    auto* tempoSyncParam = vts.getRawParameterValue("TempoSync");
    bool tempoSyncOn = (tempoSyncParam != nullptr && *tempoSyncParam > 0.5f);

    // Grey out inactive heads based on delay mode
    auto* delaySettingParam = vts.getRawParameterValue("DelaySetting");
    const int delayMode = (delaySettingParam != nullptr) ? static_cast<int>(*delaySettingParam) : 0;

    delayPanel.updateEnabledStates(tempoSyncOn, delayMode);
    delayPanel.updateHeadDisplays(tempoSyncOn, delayMode);

    // Dim the header Mix field while MixLock is engaged (mirrors the padlock)
    mixField.setLocked(mixLockValue != nullptr && mixLockValue->load() > 0.5f);

    // The scope's sweep follows the host sample rate (it ignores no-change calls).
    phosphorScope.setSampleRate(audioProcessor.getSampleRate());

    auto& display = spatialPanel.getHeadView();

    // Keep the head view in sync with automation/MIDI updates
    auto* azParam = vts.getRawParameterValue(SpatialIDs::binaural::kAzimuth);
    auto* elParam = vts.getRawParameterValue(SpatialIDs::binaural::kElevation);
    auto* distParam = vts.getRawParameterValue(SpatialIDs::binaural::kDistance);
    auto* enableParam = vts.getRawParameterValue(SpatialIDs::binaural::kEnabled);
    if (azParam && elParam && distParam && enableParam)
    {
        const float az = azParam->load();
        const float el = elParam->load();
        const float dist = distParam->load();
        const bool enabled = (*enableParam > 0.5f);

        if (std::abs(az - lastSpatialAzimuth) > 0.1f)
        {
            display.setAzimuth(az);
            lastSpatialAzimuth = az;
        }
        if (std::abs(el - lastSpatialElevation) > 0.1f)
        {
            display.setElevation(el);
            lastSpatialElevation = el;
        }
        if (std::abs(dist - lastSpatialDistance) > 0.01f)
        {
            display.setDistance(dist);
            lastSpatialDistance = dist;
        }
        if (enabled != lastSpatialEnabled)
        {
            // Keep the component interactive; toggle only radar/processing state.
            // Disabling the whole component can deaden child controls during rapid toggles.
            display.setRadarEnabled(enabled);
            lastSpatialEnabled = enabled;
        }
    }

    // Per-tap constellation sync: mode + each tap's position from APVTS
    // (automation, presets, and other UI paths all funnel through here).
    {
        if (auto* tapModeParam = vts.getRawParameterValue("HeadPositionMode"))
            display.setPerTapMode(tapModeParam->load() > 0.5f);
        for (int tap = 0; tap < 3; ++tap)
        {
            const juce::String prefix = "Head" + juce::String(tap + 1);
            auto* tAz = vts.getRawParameterValue(prefix + "Azimuth");
            auto* tEl = vts.getRawParameterValue(prefix + "Elevation");
            auto* tDist = vts.getRawParameterValue(prefix + "Distance");
            if (tAz && tEl && tDist)
                display.setTapPosition(tap, tAz->load(), tEl->load(), tDist->load());
        }
    }

    // Visual trajectory preview on radar, fed from the processor's preview
    // atomics (the pinned contract) so the UI shows what the DSP is rendering.
    SpatialHeadView::MotionPreviewState motionPreview;

    // Motion is enabled if pattern is not "Still" (pattern id 0) and Spatial is enabled.
    const int patternId = audioProcessor.getSpatialMotionPreviewPatternId();
    const bool motionEnabled = (patternId > 0) && lastSpatialEnabled;

    if (motionEnabled)
    {
        motionPreview.enabled = true;
        motionPreview.patternId = patternId;
        motionPreview.depth01 = audioProcessor.getSpatialMotionPreviewDepth01();
        motionPreview.phaseCycles = audioProcessor.getSpatialMotionPreviewPhaseCycles();
        motionPreview.blockDurationSec = audioProcessor.getSpatialMotionPreviewBlockDurationSec();
        motionPreview.currentModAz = audioProcessor.getSpatialMotionPreviewModAz();
        motionPreview.currentModEl = audioProcessor.getSpatialMotionPreviewModEl();
        motionPreview.currentModDist = audioProcessor.getSpatialMotionPreviewModDist();

        // Per-tap constellation preview inputs (trail phase offsets).
        if (auto* spreadParam = vts.getRawParameterValue(SpatialIDs::motion::kSpread))
            motionPreview.tapSpread = spreadParam->load() / 100.0f;
        if (auto* choreoParam = vts.getRawParameterValue(SpatialIDs::motion::kChoreo))
            motionPreview.tapMode = static_cast<int>(choreoParam->load());
    }
    display.setMotionPreviewState(motionPreview);

    // Dirty-state indicator: check every ~500ms (every 15 ticks at 30Hz)
    if (++dirtyCheckCounter >= 15)
    {
        dirtyCheckCounter = 0;
        if (presetStateSnapshotValid)
        {
            audioProcessor.flushPendingHostUpdatesForUi();
            const bool nowDirty = isCurrentStateDirtyAgainstSnapshot();
            if (nowDirty != presetDirty)
            {
                presetDirty = nowDirty;
                updatePresetDisplay();
                if (presetBrowser.isShowing())
                    presetBrowser.repaint();  // MODIFIED tag on the loaded row
            }
        }
    }

    // Undo transaction coalescing: group rapid changes, split on 300ms pause
    {
        auto& undo = audioProcessor.getUndoManager();
        if (undo.getNumActionsInCurrentTransaction() > 0)
            lastUndoActivityTime = juce::Time::getMillisecondCounter();

        if (lastUndoActivityTime > 0 && juce::Time::getMillisecondCounter() - lastUndoActivityTime > 300)
        {
            undo.beginNewTransaction();
            lastUndoActivityTime = 0;
        }
    }

    // Z-order: check if the About dialog state changed and reattach GL if it closed
    bool currentAboutDialogState = (aboutDialog != nullptr);
    if (currentAboutDialogState != lastAboutDialogState)
    {
        lastAboutDialogState = currentAboutDialogState;
        updateHeadViewGLForOverlays();  // Reattach GL if dialog closed and no other overlays open
    }

    walkthroughController.timerTick();
}

//==============================================================================
void SpatialEchoAudioProcessorEditor::paint(juce::Graphics& g)
{
    using namespace UIConstants;

    auto bounds = getLocalBounds();

    // Void Black background
    g.fillAll(CRTLookAndFeel::getVoidBlack());

    // Cached scanlines + gradient overlay (rebuilt only on resize)
    if (cachedBackground.isValid())
        g.drawImageAt(cachedBackground, 0, 0);

    // Section backgrounds (borders are drawn by the SectionWrapper components).
    // Prefer live section bounds so paint and layout stay in sync during resize.
    auto leftBounds = echoEngineSection.getBounds().toFloat();
    auto rightBounds = spatialSection.getBounds().toFloat();
    if (leftBounds.isEmpty() || rightBounds.isEmpty())
    {
        auto content = bounds;
        content.removeFromTop(headerHeight);
        content = content.reduced(padding);
        const int leftWidth = static_cast<int>(static_cast<float>(content.getWidth()) * 0.45f);
        leftBounds = content.removeFromLeft(leftWidth).toFloat();
        content.removeFromLeft(padding);
        rightBounds = content.toFloat();
    }

    g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.03f));
    g.fillRect(leftBounds);
    g.fillRect(rightBounds);

    // Header wordmark (FAR LEFT), IBM VGA font. It is the app-menu trigger, so
    // it brightens on hover / while the menu is open, and a one-shot shimmer
    // (advanced by the timer) plays each time the editor opens to teach the click.
    if (!headerTitleBounds.isEmpty())
    {
        const auto green = CRTLookAndFeel::getTerminalGreen();
        const auto wordmarkFont = CRTLookAndFeel::getTerminalFontStatic(headerWordmarkFontHeight);
        g.setFont(wordmarkFont);

        const float shimmer =
            wordmarkShimmerActive
                ? std::sin(juce::MathConstants<float>::pi * juce::jlimit(0.0f, 1.0f, wordmarkShimmerPhase))
                : 0.0f;
        const float glow = juce::jmax((wordmarkHovered || wordmarkMenuOpen) ? 1.0f : 0.0f, shimmer);

        const char* wordmark = headerCompactWordmark ? "SE" : "SPATIAL ECHO";

        // Phosphor bloom pass (swells as the wordmark lights up).
        g.setColour(green.withAlpha(0.20f + 0.40f * glow));
        g.drawText(wordmark, headerTitleBounds.translated(0, 1), juce::Justification::centredLeft, false);
        if (glow > 0.01f)
        {
            g.setColour(green.withAlpha(0.28f * glow));
            g.drawText(wordmark, headerTitleBounds.translated(1, 0), juce::Justification::centredLeft, false);
            g.drawText(wordmark, headerTitleBounds.translated(0, 2), juce::Justification::centredLeft, false);
        }
        // Crisp pass.
        g.setColour(green.withAlpha(0.90f + 0.10f * glow));
        g.drawText(wordmark, headerTitleBounds, juce::Justification::centredLeft, false);

        // Dropdown caret. Drawn as a path because the
        // IBM VGA font has no arrow glyphs; it flips while the menu is open.
        const float caretW = headerWordmarkFontHeight * kWordmarkCaretWidthRatio;
        const float caretH = caretW * 0.55f;
        const float caretX = static_cast<float>(headerTitleBounds.getX()) +
                             static_cast<float>(juce::GlyphArrangement::getStringWidthInt(wordmarkFont, wordmark)) +
                             headerWordmarkFontHeight * kWordmarkCaretGapRatio;
        const float caretY = static_cast<float>(headerTitleBounds.getCentreY()) - caretH * 0.5f;
        juce::Path caret;
        if (wordmarkMenuOpen)
            caret.addTriangle(caretX, caretY + caretH, caretX + caretW * 0.5f, caretY, caretX + caretW,
                              caretY + caretH);
        else
            caret.addTriangle(caretX, caretY, caretX + caretW * 0.5f, caretY + caretH, caretX + caretW, caretY);
        g.setColour(green.withAlpha(0.55f + 0.45f * glow));
        g.fillPath(caret);
    }

    // ===== Preset Bar Frame (etched with legend) =====
    // Frame encompasses prev button, name field, next button. Legend "PRESET" is etched
    // into the top border with a gap-in-stroke effect (legend-on-frame idiom).
    if (!presetPrevButton.getBounds().isEmpty() && !presetNextButton.getBounds().isEmpty())
    {
        auto prevBounds = presetPrevButton.getBounds().toFloat();
        auto nextBounds = presetNextButton.getBounds().toFloat();

        // Frame encloses all three controls; the top margin is deeper so the
        // etched legend clears the name text instead of straddling into it.
        // 6px fits: headerInner leaves 7px above the controls.
        const float margin = 2.0f;
        const float topMargin = 6.0f;
        auto frameBounds = prevBounds.withLeft(prevBounds.getX() - margin)
                               .withRight(nextBounds.getRight() + margin)
                               .withTop(prevBounds.getY() - topMargin)
                               .withBottom(prevBounds.getBottom() + margin);

        const auto green = CRTLookAndFeel::getTerminalGreen();
        const float cornerRadius = 2.0f;

        // Draw frame border (except top, which is broken for legend)
        g.setColour(green.withAlpha(0.35f));

        // Left side
        g.drawLine(frameBounds.getX(), frameBounds.getY() + cornerRadius, frameBounds.getX(), frameBounds.getBottom(),
                   1.0f);

        // Right side
        g.drawLine(frameBounds.getRight(), frameBounds.getY() + cornerRadius, frameBounds.getRight(),
                   frameBounds.getBottom(), 1.0f);

        // Bottom side
        g.drawLine(frameBounds.getX() + cornerRadius, frameBounds.getBottom(), frameBounds.getRight() - cornerRadius,
                   frameBounds.getBottom(), 1.0f);

        // Top side (broken for legend):
        // Draw left portion, gap for legend text, then right portion
        const auto legendFont = CRTLookAndFeel::getTerminalFontStatic(kPresetLegendFontSize);
        const juce::String legendText = "PRESET";
        const float legendTextW = static_cast<float>(juce::GlyphArrangement::getStringWidthInt(legendFont, legendText));
        const float legendGapW = legendTextW + 12.0f;  // text width + padding
        const float legendCentreX = frameBounds.getCentreX();
        const float legendLeft = legendCentreX - legendGapW / 2.0f;
        const float legendRight = legendCentreX + legendGapW / 2.0f;
        const float topY = frameBounds.getY();

        // Left portion of top border
        g.drawLine(frameBounds.getX() + cornerRadius, topY, legendLeft, topY, 1.0f);

        // Right portion of top border
        g.drawLine(legendRight, topY, frameBounds.getRight() - cornerRadius, topY, 1.0f);

        // Legend text (dim phosphor, etched look). 10px box centred on the
        // raised border: glyphs clear both the strip edge above and the
        // name text below.
        g.setColour(green.withAlpha(0.5f));
        g.setFont(legendFont);
        g.drawText(legendText, juce::Rectangle<float>(legendLeft, topY - 5.0f, legendGapW, 10.0f),
                   juce::Justification::centred, false);

        // Top corner accents (CRT aesthetic).
        g.setColour(green.withAlpha(0.5f));
        g.drawLine(frameBounds.getX(), frameBounds.getY(), frameBounds.getX() + cornerRadius, frameBounds.getY(), 1.0f);
        g.drawLine(frameBounds.getX(), frameBounds.getY(), frameBounds.getX(), frameBounds.getY() + cornerRadius, 1.0f);

        g.drawLine(frameBounds.getRight(), frameBounds.getY(), frameBounds.getRight() - cornerRadius,
                   frameBounds.getY(), 1.0f);
        g.drawLine(frameBounds.getRight(), frameBounds.getY(), frameBounds.getRight(),
                   frameBounds.getY() + cornerRadius, 1.0f);
    }
}

void SpatialEchoAudioProcessorEditor::resized()
{
    using namespace UIConstants;
    auto bounds = getLocalBounds();
    const int dynHeaderHeight = scopeVisible ? headerHeight : headerControlStripHeight;
    auto headerArea = bounds.removeFromTop(dynHeaderHeight);

    // Split header: top control strip | bottom phosphor scope
    auto controlStripArea = headerArea.removeFromTop(headerControlStripHeight);
    if (scopeVisible)
        phosphorScope.setBounds(headerArea);  // remaining lower portion

    // ---- Header layout: wordmark (left) | TAPE (over ECHO ENGINE) |
    //      preset bar (centred) | MIX (far right). ----
    auto headerInner = controlStripArea.reduced(padding, smallPadding);
    const int controlHeight = juce::jlimit(24, 34, headerInner.getHeight());
    const int dropdownY = headerInner.getY() + (headerInner.getHeight() - controlHeight) / 2;

    // SPATIAL ECHO wordmark, far left, sized to fill the strip. The font height
    // is stored so paint() renders identical metrics.
    headerWordmarkFontHeight = juce::jlimit(24.0f, 40.0f, static_cast<float>(controlStripArea.getHeight()) * 0.72f);
    const auto titleFont = CRTLookAndFeel::getTerminalFontStatic(headerWordmarkFontHeight);
    auto layoutWordmark = [&](bool compact)
    {
        const char* wordmark = compact ? "SE" : "SPATIAL ECHO";
        const int textW = juce::jmax(1, juce::GlyphArrangement::getStringWidthInt(titleFont, wordmark));
        const int caretW =
            juce::roundToInt(headerWordmarkFontHeight * (kWordmarkCaretGapRatio + kWordmarkCaretWidthRatio));
        headerCompactWordmark = compact;
        headerTitleBounds = {headerInner.getX(), controlStripArea.getY(), textW + caretW + panelGap * 2,
                             controlStripArea.getHeight()};
    };
    layoutWordmark(false);

    // The full wordmark fits at every editor size; the "SE" form is a safety
    // net should the preset bar ever crowd it.

    // OUT + MIX + padlock take the far-right slot (both always visible - mission-critical).
    // Padlock at the 24px minimum hit target
    const int lockW = 24;
    const int outW = outField.getPreferredWidth();
    const int mixW = mixField.getPreferredWidth();
    const int fieldGap = 6;
    const int groupGap = 18;  // OUT and MIX are separate clusters (6px read as one blob)
    outField.setVisible(true);
    mixField.setVisible(true);
    mixLockButton.setVisible(true);

    // Position: [OUT] gap [MIX padlock]
    const int rightEdge = headerInner.getRight();
    mixLockButton.setBounds(rightEdge - lockW, dropdownY + (controlHeight - lockW) / 2, lockW, lockW);
    const int mixX = mixLockButton.getX() - fieldGap - mixW;
    mixField.setBounds(mixX, dropdownY, mixW, controlHeight);
    const int outX = mixX - groupGap - outW;
    outField.setBounds(outX, dropdownY, outW, controlHeight);

    // Preset bar (< name >, PRESET etched into the frame) - the header's focal
    // element, truly centred on the window midpoint, clamped against the
    // wordmark (left) and the OUT field (right). The invisible PRESET label
    // must not be counted in the centring math: that biases the visible bar
    // ~32px right of centre.
    const int navW = 28;  // WCAG 2.5.8: minimum 24x24 target size
    int spanLeft = headerTitleBounds.getRight() + panelGap * 2;
    const int spanRight = outField.getX() - panelGap * 2;
    const int fixedW = navW + navW + panelGap * 2;
    int maxNameW = juce::jmax(120, spanRight - spanLeft - fixedW);
    if (maxNameW < 200)
    {
        // Crowded: degrade the wordmark to "SE" before shrinking the bar.
        layoutWordmark(true);
        spanLeft = headerTitleBounds.getRight() + panelGap * 2;
        maxNameW = juce::jmax(120, spanRight - spanLeft - fixedW);
    }
    const int nameW = juce::jlimit(320, 560, juce::jmin(headerInner.getWidth() / 4, maxNameW));
    const int barW = fixedW + nameW;
    int barX = juce::jlimit(spanLeft, juce::jmax(spanLeft, spanRight - barW), headerInner.getCentreX() - barW / 2);

    presetPrevButton.setBounds(barX, dropdownY, navW, controlHeight);
    barX += navW + panelGap;
    presetNameButton.setBounds(barX, dropdownY, nameW, controlHeight);
    barX += nameW + panelGap;
    presetNextButton.setBounds(barX, dropdownY, navW, controlHeight);

    // Invisible click target sits exactly over the painted wordmark.
    brandButton.setBounds(headerTitleBounds);

    // Main content split
    auto content = bounds.reduced(padding);
    const int sectionGap = panelGap;
    auto leftSection = content.removeFromLeft((content.getWidth() - sectionGap) / 2);
    content.removeFromLeft(sectionGap);
    auto rightSection = content;
    echoEngineSection.setBounds(leftSection);
    spatialSection.setBounds(rightSection);

    auto leftInner = leftSection.reduced(padding);
    auto rightInner = rightSection.reduced(padding);
    const int verticalGap = panelGap;

    auto delayArea = leftInner.removeFromTop((leftInner.getHeight() - verticalGap) * 61 / 100);
    leftInner.removeFromTop(verticalGap);
    auto reverbArea = leftInner;

    delayPanel.setBounds(delayArea);
    reverbPanel.setBounds(reverbArea);
    spatialPanel.setBounds(rightInner);

    // TAPE group scales with the delay panel's base grid (same as
    // DelayPanel::resized) so it stays aligned at every window size.
    // Frameless drawer-row idiom: TAPE caption over the flavor combo, then a
    // full-width horizontal DRIVE slider.
    {
        const UIConstants::GridScaler tapeScaled(delayArea, delayGridWidth, delayGridHeight);
        tapeLabel.setBounds(tapeScaled(8, 2, 156, 14));
        auto flavorBounds = tapeScaled(8, 18, 156, 24);
        flavorBounds.setWidth(juce::jmax(flavorBounds.getWidth(), tapeFlavorBox.getRequiredValueWidth()));
        tapeFlavorBox.setBounds(flavorBounds);
        tapeDriveSlider.setBounds(tapeScaled(8, 50, 156, 24));
    }

    echoEngineDropdown.setBounds(delayArea.getCentreX() - 60, delayArea.getY() + 8, 120, 28);
    reverbDropdown.setBounds(reverbArea.getCentreX() - 50, reverbArea.getY() + 8, 100, 28);

#if JUCE_DEBUG
    // Keep critical controls visible at current size constraints.
    const auto editorBounds = getLocalBounds();
    jassert(editorBounds.contains(presetNameButton.getBounds()));
#endif

    // Help overlay + preset browser cover the full editor
    helpOverlay.setBounds(getLocalBounds());
    presetBrowser.setBounds(getLocalBounds());
    interactiveWalkthrough.setBounds(getLocalBounds());
    // Panel placement: the left content half never intersects the GL head
    // view (right half), so the head stays attached and live while the
    // browser is open.
    presetBrowser.setPanelPlacementArea(leftSection);

    // Rebuild cached background image (scanlines + gradient; only on resize)
    {
        auto fullBounds = getLocalBounds();
        cachedBackground = juce::Image(juce::Image::ARGB, fullBounds.getWidth(), fullBounds.getHeight(), true);
        juce::Graphics bg(cachedBackground);
        CRTLookAndFeel::drawGradientOverlay(bg, fullBounds);
        CRTLookAndFeel::drawScanlines(bg, fullBounds);
    }

    // Buffer static section wrappers (borders rarely change)
    echoEngineSection.setBufferedToImage(true);
    spatialSection.setBufferedToImage(true);

    if (interactiveWalkthrough.isVisible())
        interactiveWalkthrough.refreshTarget();
}

bool SpatialEchoAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (interactiveWalkthrough.isVisible() && interactiveWalkthrough.keyPressed(key))
        return true;

    // Undo / redo: Cmd+Z / Shift+Cmd+Z on macOS, Ctrl+Z / Ctrl+Shift+Z elsewhere.
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z')
    {
        SpatialEchoAudioProcessor::ScopedSpatialParameterBatch batch(audioProcessor);
        auto& undo = audioProcessor.getUndoManager();
        if (key.getModifiers().isShiftDown())
            undo.redo();
        else
            undo.undo();
        return true;
    }

    // Help overlay toggle (? key: Shift+/ on US layout, or direct '?' character)
    if (key.getTextCharacter() == '?')
    {
        helpOverlay.toggle();
        helpOverlay.setBounds(getLocalBounds());
        updateHeadViewGLForOverlays();
        return true;
    }

    return false;
}

void SpatialEchoAudioProcessorEditor::visibilityChanged()
{
    walkthroughController.scheduleAutomaticStartIfEligible();
}

void SpatialEchoAudioProcessorEditor::refreshPresetList()
{
    cachedFactoryPresetNames = audioProcessor.getFactoryPresetNames();
    cachedPresetNames = audioProcessor.getUserPresetNames();

    if (currentPresetType == PresetSelectionType::Factory)
    {
        if (currentPresetName.isEmpty() || !cachedFactoryPresetNames.contains(currentPresetName))
        {
            currentPresetType = PresetSelectionType::Init;
            currentPresetName.clear();
        }
    }
    else if (currentPresetType == PresetSelectionType::User)
    {
        if (currentPresetName.isNotEmpty() && !cachedPresetNames.contains(currentPresetName))
        {
            const auto sanitized = sanitizePresetNameForUi(currentPresetName);
            if (cachedPresetNames.contains(sanitized))
                currentPresetName = sanitized;
            else
            {
                currentPresetType = PresetSelectionType::Init;
                currentPresetName.clear();
            }
        }
    }
    else
    {
        currentPresetName.clear();
    }

    updatePresetDisplay();
}

void SpatialEchoAudioProcessorEditor::updatePresetDisplay()
{
    const bool hasPresets = (cachedFactoryPresetNames.size() + cachedPresetNames.size()) > 0;
    presetPrevButton.setEnabled(hasPresets);
    presetNextButton.setEnabled(hasPresets);

    juce::String displayName = "Init";
    if (currentPresetType != PresetSelectionType::Init && currentPresetName.isNotEmpty())
        displayName = currentPresetName;

    if (presetDirty)
        displayName += " *";
    presetNameButton.setButtonText(displayName);

    // Persist active preset identity to ValueTree so it survives
    // getStateInformation / setStateInformation and editor recreation.
    auto& state = audioProcessor.getValueTreeState().state;
    state.setProperty("activePresetName", currentPresetName, nullptr);
    state.setProperty("activePresetType", static_cast<int>(currentPresetType), nullptr);
}

void SpatialEchoAudioProcessorEditor::loadPresetSelection(PresetSelectionType type, const juce::String& name,
                                                          const juce::String& transactionName)
{
    if (transactionName.isNotEmpty())
        audioProcessor.getUndoManager().beginNewTransaction(transactionName);

    switch (type)
    {
        case PresetSelectionType::Init:
            audioProcessor.loadDefaultPreset();
            currentPresetType = PresetSelectionType::Init;
            currentPresetName.clear();
            reverbDropdown.resyncTypeComboFromPreset();
            break;

        case PresetSelectionType::Factory:
            if (!audioProcessor.factoryPresetExists(name))
                return;
            audioProcessor.loadFactoryPreset(name);
            currentPresetType = PresetSelectionType::Factory;
            currentPresetName = name;
            reverbDropdown.resyncTypeComboFromPreset();
            break;

        case PresetSelectionType::User:
            if (!audioProcessor.userPresetExists(name))
                return;
            audioProcessor.loadUserPreset(name);
            currentPresetType = PresetSelectionType::User;
            currentPresetName = name;
            reverbDropdown.resyncTypeComboFromPreset();
            break;
    }

    notifyHostOfPresetParameterValues(audioProcessor.getValueTreeState());
    capturePresetSnapshot();
}

void SpatialEchoAudioProcessorEditor::capturePresetSnapshot()
{
    audioProcessor.flushPendingHostUpdatesForUi();
    auto& vts = audioProcessor.getValueTreeState();

    presetStateSnapshotValid = false;
    for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
    {
        const auto* parameterId = FactoryPresetLibrary::kParameterIds[i];
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(vts.getParameter(parameterId));

        presetStateSnapshotTracked[i] = false;
        presetStateSnapshotDiscrete[i] = false;
        presetStateSnapshotEpsilons[i] = 0.0f;
        presetStateSnapshotValues[i] = 0.0f;

        if (ranged == nullptr)
            continue;

        const bool discrete = shouldUseDiscretePresetComparison(*ranged);
        presetStateSnapshotTracked[i] = true;
        presetStateSnapshotDiscrete[i] = discrete;
        presetStateSnapshotEpsilons[i] = getPresetContinuousComparisonEpsilon(*ranged);
        presetStateSnapshotValues[i] = getPresetPlainValue(*ranged, discrete);
        presetStateSnapshotValid = true;
    }

    presetDirty = false;
    dirtyCheckCounter = 0;
    updatePresetDisplay();
}

bool SpatialEchoAudioProcessorEditor::isCurrentStateDirtyAgainstSnapshot() const
{
    if (!presetStateSnapshotValid)
        return false;

    auto& vts = audioProcessor.getValueTreeState();
    for (size_t i = 0; i < FactoryPresetLibrary::kParameterIds.size(); ++i)
    {
        if (!presetStateSnapshotTracked[i])
            continue;

        const auto* parameterId = FactoryPresetLibrary::kParameterIds[i];
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(vts.getParameter(parameterId));
        if (ranged == nullptr)
            return true;

        const bool discrete = presetStateSnapshotDiscrete[i];
        const float currentValue = getPresetPlainValue(*ranged, discrete);
        if (!std::isfinite(currentValue))
            return true;

        const float snapshotValue = presetStateSnapshotValues[i];
        if (discrete)
        {
            if (juce::roundToInt(currentValue) != juce::roundToInt(snapshotValue))
                return true;
            continue;
        }

        const float delta = std::abs(currentValue - snapshotValue);
        if (delta > presetStateSnapshotEpsilons[i])
            return true;
    }

    return false;
}

bool SpatialEchoAudioProcessorEditor::shouldUseDiscretePresetComparison(const juce::RangedAudioParameter& parameter)
{
    return dynamic_cast<const juce::AudioParameterBool*>(&parameter) != nullptr ||
           dynamic_cast<const juce::AudioParameterChoice*>(&parameter) != nullptr ||
           dynamic_cast<const juce::AudioParameterInt*>(&parameter) != nullptr;
}

float SpatialEchoAudioProcessorEditor::getPresetContinuousComparisonEpsilon(const juce::RangedAudioParameter& parameter)
{
    if (auto* floatParameter = dynamic_cast<const juce::AudioParameterFloat*>(&parameter))
    {
        const auto& range = floatParameter->range;
        if (range.interval > 0.0f)
            return juce::jmax(1.0e-6f, range.interval * 0.5f);

        const float span = std::abs(range.end - range.start);
        return juce::jmax(1.0e-6f, span * 1.0e-4f);
    }

    return 1.0e-4f;
}

void SpatialEchoAudioProcessorEditor::selectRelativePreset(int delta)
{
    refreshPresetList();

    struct PresetNavEntry
    {
        PresetSelectionType type;
        juce::String name;
    };

    juce::Array<PresetNavEntry> entries;
    entries.add({PresetSelectionType::Init, {}});
    for (const auto& name : cachedFactoryPresetNames)
        entries.add({PresetSelectionType::Factory, name});
    for (const auto& name : cachedPresetNames)
        entries.add({PresetSelectionType::User, name});

    if (entries.size() <= 1)
        return;

    int currentIndex = 0;
    for (int i = 0; i < entries.size(); ++i)
    {
        const auto& entry = entries.getReference(i);
        if (entry.type == currentPresetType && entry.name == currentPresetName)
        {
            currentIndex = i;
            break;
        }
    }

    const int nextIndex = (currentIndex + delta + entries.size()) % entries.size();
    const auto& next = entries.getReference(nextIndex);
    const auto transaction = (next.type == PresetSelectionType::Init) ? "Load Init Preset" : "Load Preset";
    loadPresetSelection(next.type, next.name, transaction);
}

void SpatialEchoAudioProcessorEditor::showPresetBrowser()
{
    refreshPresetList();

    // Browse by audible purpose; the authoring categories remain searchable.
    // Init is the browser's INIT action, and User is always listed, even when
    // empty. Each item carries its own load closure so the overlay stays
    // decoupled from the preset system.
    std::vector<PresetBrowserOverlay::Category> model;

    {
        PresetBrowserOverlay::Category init;
        init.label = "Init";
        init.isInit = true;
        init.items.push_back({"Init",
                              FactoryPresetDescriptions::kInitDescription,
                              [this]() { loadPresetSelection(PresetSelectionType::Init, {}, "Load Init Preset"); },
                              false,
                              {}});
        model.push_back(std::move(init));
    }

    auto makeFactoryItem = [this](const juce::String& name, const juce::String& authoringTerms = {})
    {
        const juce::String description(FactoryPresetDescriptions::find(name.toRawUTF8()));
        return PresetBrowserOverlay::Item{
            name, description,
            [this, name]() { loadPresetSelection(PresetSelectionType::Factory, name, "Load Factory Preset"); }, false,
            juce::String(FactoryPresetDescriptions::findSearchTerms(name.toRawUTF8())) + " " + authoringTerms};
    };

    std::array<PresetBrowserOverlay::Category, FactoryPresetBrowseGroups::kGroupLabels.size()> factoryCategories;
    for (size_t i = 0; i < factoryCategories.size(); ++i)
        factoryCategories[i].label = FactoryPresetBrowseGroups::kGroupLabels[i];
    juce::StringArray groupedNames;

    for (const auto& authoringCategory : FactoryPresetLibrary::kFactoryPresetCategories)
    {
        for (const auto& definition : FactoryPresetLibrary::kFactoryPresets)
        {
            if (juce::String(definition.category) != authoringCategory.id)
                continue;
            const juce::String name(definition.name);
            if (!cachedFactoryPresetNames.contains(name))
                continue;

            groupedNames.add(name);

            const auto authoringTerms = juce::String(authoringCategory.id) + " " + authoringCategory.label;
            const int group = FactoryPresetBrowseGroups::groupIndexFor(definition.name);
            jassert(group >= 0);
            // Coverage guards the authored roster. An unknown runtime factory
            // name remains reachable rather than disappearing from the browser.
            const auto index = group >= 0 ? static_cast<size_t>(group) : factoryCategories.size() - 1;
            factoryCategories[index].items.push_back(makeFactoryItem(name, authoringTerms));
        }
    }

    for (const auto& name : cachedFactoryPresetNames)
    {
        if (!groupedNames.contains(name))
            factoryCategories.back().items.push_back(makeFactoryItem(name));
    }

    for (auto& category : factoryCategories)
    {
        model.push_back(std::move(category));
    }

    {
        PresetBrowserOverlay::Category user;
        user.label = "User";
        for (const auto& name : cachedPresetNames)
        {
            user.items.push_back({name,
                                  juce::String(),
                                  [this, name]()
                                  { loadPresetSelection(PresetSelectionType::User, name, "Load Preset"); },
                                  /*userEditable*/ true,
                                  {}});
        }
        model.push_back(std::move(user));
    }

    // Wire the action bar / inline affordances to the shared preset helpers.
    PresetBrowserOverlay::Callbacks callbacks;
    // Polled at paint time: auditioning keeps the browser open, so a bool
    // snapshot would go stale the moment a different preset is loaded.
    callbacks.canSave = [this]()
    { return currentPresetType == PresetSelectionType::User && currentPresetName.isNotEmpty(); };
    callbacks.isDirty = [this]() { return presetDirty; };
    callbacks.onSave = [this]() { presetSave(); };
    callbacks.onSaveAs = [this]() { presetSaveAs(); };
    callbacks.onImport = [this]() { presetImport(); };
    callbacks.onExport = [this]() { presetExport(); };
    callbacks.onReveal = [this]() { presetRevealFolder(); };
    callbacks.onRename = [this](const juce::String& n) { presetRename(n); };
    callbacks.onDelete = [this](const juce::String& n) { presetDelete(n); };
    callbacks.onLibraryMenuOpened = [this]()
    {
        interactiveWalkthrough.setExternalPopupActive(true);
        interactiveWalkthrough.attachToCurrentPopupMenu();
    };
    callbacks.onLibraryMenuClosed = [this]()
    {
        interactiveWalkthrough.detachFromModalComponent();
        interactiveWalkthrough.setExternalPopupActive(walkthroughController.isAnyMenuVisible());
    };
    callbacks.onDismiss = [this]()
    {
        interactiveWalkthrough.setSpotlightFocusComponent(nullptr);
        updateHeadViewGLForOverlays();
    };
    presetBrowser.setCallbacks(std::move(callbacks));

    const juce::String currentName = (currentPresetType == PresetSelectionType::Init || currentPresetName.isEmpty())
                                         ? juce::String("Init")
                                         : currentPresetName;
    if (interactiveWalkthrough.isVisible())
        interactiveWalkthrough.setSpotlightFocusComponent(&presetBrowser);
    presetBrowser.show(std::move(model), currentName);
    if (interactiveWalkthrough.isVisible())
    {
        interactiveWalkthrough.toFront(false);
        interactiveWalkthrough.refreshTarget();
    }
    updateHeadViewGLForOverlays();
}

// ---- Preset actions (shared by the browser's action bar + inline glyphs) ----

void SpatialEchoAudioProcessorEditor::presetSave()
{
    if (currentPresetType != PresetSelectionType::User || currentPresetName.isEmpty())
        return;
    if (!audioProcessor.saveUserPreset(currentPresetName))
    {
        showPresetMessage("Save Failed", "Unable to save preset. Your changes have not been saved.");
        return;
    }
    currentPresetType = PresetSelectionType::User;
    currentPresetName = sanitizePresetNameForUi(currentPresetName);
    refreshPresetList();
    capturePresetSnapshot();
}

void SpatialEchoAudioProcessorEditor::presetSaveAs()
{
    const juce::String activeName = (currentPresetType == PresetSelectionType::Init || currentPresetName.isEmpty())
                                        ? juce::String("Init")
                                        : currentPresetName;

    promptForPresetName(
        "Save Preset", activeName, "Save",
        [this](const juce::String& name)
        {
            auto performSave = [this, name]()
            {
                if (!audioProcessor.saveUserPreset(name))
                {
                    showPresetMessage("Save Failed", "Unable to save preset. Your changes have not been saved.");
                    return;
                }
                currentPresetType = PresetSelectionType::User;
                currentPresetName = sanitizePresetNameForUi(name);
                refreshPresetList();
                capturePresetSnapshot();
            };

            if (audioProcessor.userPresetExists(name))
            {
                confirmPresetAction("Overwrite Preset", "A preset with this name already exists. Overwrite it?",
                                    "Overwrite",
                                    [performSave](bool confirmed)
                                    {
                                        if (confirmed)
                                            performSave();
                                    });
                return;
            }

            performSave();
        });
}

void SpatialEchoAudioProcessorEditor::presetExport()
{
    const juce::String activeName = (currentPresetType == PresetSelectionType::Init || currentPresetName.isEmpty())
                                        ? juce::String("Init")
                                        : currentPresetName;

    const auto startDir =
        presetLastDirectory.isDirectory() ? presetLastDirectory : audioProcessor.getUserPresetDirectoryOnDisk();
    auto startFile = startDir.getChildFile(activeName).withFileExtension(kPresetFileExtension);
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);
    presetFileChooser =
        std::make_unique<juce::FileChooser>("Export Preset", startFile, "*." + juce::String(kPresetFileExtension));
    presetFileChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            auto file = chooser.getResult();
            if (file == juce::File())
                return;

            safeThis->presetLastDirectory = file.getParentDirectory();
            if (!file.hasFileExtension(kPresetFileExtension))
                file = file.withFileExtension(kPresetFileExtension);

            const auto nameForWrapper =
                (safeThis->currentPresetType != PresetSelectionType::Init && safeThis->currentPresetName.isNotEmpty())
                    ? safeThis->currentPresetName
                    : file.getFileNameWithoutExtension();
            if (!safeThis->audioProcessor.exportUserPresetToFile(file, nameForWrapper))
                safeThis->showPresetMessage("Export Failed", "Unable to export preset file.");
        });
}

void SpatialEchoAudioProcessorEditor::presetImport()
{
    const auto startDir =
        presetLastDirectory.isDirectory() ? presetLastDirectory : audioProcessor.getUserPresetDirectoryOnDisk();
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);
    presetFileChooser =
        std::make_unique<juce::FileChooser>("Import Preset", startDir, "*." + juce::String(kPresetFileExtension));
    presetFileChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;

            const auto file = chooser.getResult();
            if (!file.existsAsFile())
                return;

            safeThis->presetLastDirectory = file.getParentDirectory();

            const auto incomingName = safeThis->audioProcessor.getPresetNameFromFile(file);
            auto performImport = [safeThis, file]()
            {
                if (safeThis == nullptr)
                    return;

                juce::String importedName;
                if (!safeThis->audioProcessor.importUserPresetFromFile(file, &importedName))
                {
                    safeThis->showPresetMessage("Import Failed", "Unable to import preset file.");
                    return;
                }

                safeThis->currentPresetType = PresetSelectionType::User;
                safeThis->currentPresetName = importedName;
                safeThis->refreshPresetList();
                safeThis->capturePresetSnapshot();
            };

            if (incomingName.isNotEmpty() && safeThis->audioProcessor.userPresetExists(incomingName))
            {
                const auto message = "A preset named \"" + incomingName + "\" already exists. Overwrite it?";
                safeThis->confirmPresetAction("Overwrite Preset", message, "Overwrite",
                                              [performImport](bool confirmed)
                                              {
                                                  if (confirmed)
                                                      performImport();
                                              });
                return;
            }

            performImport();
        });
}

void SpatialEchoAudioProcessorEditor::presetRevealFolder()
{
    auto folder = audioProcessor.getUserPresetDirectoryOnDisk();
    folder.createDirectory();
    folder.startAsProcess();
}

void SpatialEchoAudioProcessorEditor::presetRename(const juce::String& existingName)
{
    if (existingName.isEmpty())
        return;

    promptForPresetName("Rename Preset", existingName, "Rename",
                        [this, existingName](const juce::String& name)
                        {
                            const auto sanitized = sanitizePresetNameForUi(name);
                            if (sanitized.isEmpty())
                            {
                                showPresetMessage("Invalid Name", "Preset name cannot be empty.");
                                return;
                            }
                            if (sanitized == existingName)
                                return;
                            if (audioProcessor.userPresetExists(sanitized))
                            {
                                showPresetMessage("Preset Exists", "A preset with that name already exists.");
                                return;
                            }
                            if (!audioProcessor.renameUserPreset(existingName, sanitized))
                            {
                                showPresetMessage("Rename Failed", "Unable to rename preset.");
                                return;
                            }
                            if (currentPresetName == existingName)
                            {
                                currentPresetName = sanitized;
                                currentPresetType = PresetSelectionType::User;
                            }
                            refreshPresetList();
                            capturePresetSnapshot();
                        });
}

void SpatialEchoAudioProcessorEditor::presetDelete(const juce::String& presetName)
{
    if (presetName.isEmpty())
        return;

    const auto message = "Delete preset \"" + presetName + "\"? This cannot be undone.";
    confirmPresetAction("Delete Preset", message, "Delete",
                        [this, presetName](bool confirmed)
                        {
                            if (!confirmed)
                                return;

                            if (!audioProcessor.deleteUserPreset(presetName))
                            {
                                showPresetMessage("Delete Failed", "Unable to delete preset.");
                                return;
                            }

                            if (currentPresetName == presetName)
                            {
                                currentPresetType = PresetSelectionType::Init;
                                currentPresetName.clear();
                            }
                            refreshPresetList();
                            capturePresetSnapshot();
                        });
}

// ---- Tape flavor menu (flavors plus the Noise override) ----

void SpatialEchoAudioProcessorEditor::showTapeFlavorMenu()
{
    const int generation = walkthroughController.beginTapeMenu();
    walkthroughController.setTapeMenuVisible(true);
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getSharedPopupLookAndFeel());

    const int selectedId = tapeFlavorBox.getSelectedId();
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);
    for (int i = 0; i < tapeFlavorBox.getNumItems(); ++i)
    {
        const int itemId = i + 1;
        menu.addItem(tapeFlavorBox.getItemText(i), true, itemId == selectedId,
                     [safeThis, itemId]
                     {
                         if (safeThis != nullptr)
                             safeThis->tapeFlavorBox.setSelectedId(itemId, juce::sendNotificationSync);
                     });
    }

    menu.addSeparator();
    if (auto* noiseParameter = audioProcessor.getValueTreeState().getParameter("TapeNoiseEnabled"))
    {
        const bool noiseEnabled = noiseParameter->getValue() > 0.5f;
        menu.addItem("Noise", true, noiseEnabled,
                     [noiseParameter, noiseEnabled]
                     {
                         noiseParameter->beginChangeGesture();
                         noiseParameter->setValueNotifyingHost(noiseEnabled ? 0.0f : 1.0f);
                         noiseParameter->endChangeGesture();
                     });
    }

    menu.showMenuAsync(MenuChrome::popupOptions(tapeFlavorBox),
                       [safeThis, generation](int)
                       {
                           if (safeThis != nullptr)
                               safeThis->completeTapeFlavorMenu(generation);
                       });
    interactiveWalkthrough.attachToCurrentPopupMenu();
}

void SpatialEchoAudioProcessorEditor::completeTapeFlavorMenu(int generation)
{
    if (!walkthroughController.isCurrentTapeMenu(generation))
        return;

    interactiveWalkthrough.detachFromModalComponent();
    tapeFlavorBox.hidePopup();
    walkthroughController.setTapeMenuVisible(false);
    if (interactiveWalkthrough.isVisible())
        interactiveWalkthrough.toFront(false);
}

void SpatialEchoAudioProcessorEditor::setScaleFactor(float newScale)
{
    hostScaleFactor = newScale;
    AudioProcessorEditor::setScaleFactor(hostScaleFactor * windowZoom);
}

void SpatialEchoAudioProcessorEditor::setWindowZoom(float zoom)
{
    windowZoom = juce::jlimit(0.75f, 1.0f, zoom);
    (void)ensureEditorPreferences().setWindowZoom(windowZoom);
    AudioProcessorEditor::setScaleFactor(hostScaleFactor * windowZoom);
}

EditorPreferences& SpatialEchoAudioProcessorEditor::ensureEditorPreferences()
{
    if (editorPreferences == nullptr)
        editorPreferences = std::make_unique<EditorPreferences>();
    return *editorPreferences;
}

EditorPreferences* SpatialEchoAudioProcessorEditor::storedPreferences()
{
    if (editorPreferences == nullptr && audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_Undefined)
        return nullptr;
    return &ensureEditorPreferences();
}

// ---- Wordmark app menu (settings, help, about) ----

void SpatialEchoAudioProcessorEditor::showAppMenu()
{
    walkthroughController.setAppMenuVisible(true);
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getSharedPopupLookAndFeel());

    // Undo / Redo (top of menu), with the platform's shortcut on the right.
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);
    auto& undoManager = audioProcessor.getUndoManager();
    auto addHistoryItem =
        [&menu, safeThis](const juce::String& text, const juce::String& shortcut, bool enabled, bool redo)
    {
        juce::PopupMenu::Item item(text);
        item.setEnabled(enabled);
        item.shortcutKeyDescription = shortcut;
        item.setAction(
            [safeThis, redo]
            {
                if (safeThis == nullptr)
                    return;
                SpatialEchoAudioProcessor::ScopedSpatialParameterBatch batch(safeThis->audioProcessor);
                auto& history = safeThis->audioProcessor.getUndoManager();
                if (redo)
                    history.redo();
                else
                    history.undo();
            });
        menu.addItem(std::move(item));
    };
    addHistoryItem("Undo", EditorShortcuts::undoKeyText(), undoManager.canUndo(), false);
    addHistoryItem("Redo", EditorShortcuts::redoKeyText(), undoManager.canRedo(), true);

    menu.addSeparator();

    // Oscilloscope toggle
    menu.addItem("Oscilloscope", true, scopeVisible,
                 [this]()
                 {
                     scopeVisible = !scopeVisible;
                     phosphorScope.setVisible(scopeVisible);
                     audioProcessor.getValueTreeState().state.setProperty("scopeVisible", scopeVisible, nullptr);
                     resized();
                 });

    // Tooltips toggle
    menu.addItem("Tooltips", true, tooltipWindow.enabled, [this]() { tooltipWindow.enabled = !tooltipWindow.enabled; });

    // Window Size: whole-interface zoom (layout unchanged, so nothing truncates).
    juce::PopupMenu windowSizeMenu;
    windowSizeMenu.setLookAndFeel(&getSharedPopupLookAndFeel());
    for (const float zoom : {1.0f, 0.75f})
    {
        windowSizeMenu.addItem(juce::String(juce::roundToInt(zoom * 100.0f)) + "%", true,
                               std::abs(windowZoom - zoom) < 0.01f,
                               [safeThis, zoom]()
                               {
                                   if (safeThis != nullptr)
                                       safeThis->setWindowZoom(zoom);
                               });
    }
    menu.addSubMenu("Window Size", windowSizeMenu);

    // Output Protection (peak safety limiter)
    if (auto* protectParam = audioProcessor.getValueTreeState().getParameter("OutputProtection"))
    {
        const bool protectOn = protectParam->getValue() > 0.5f;
        menu.addItem("Output Protection", true, protectOn,
                     [protectParam, protectOn]()
                     {
                         protectParam->beginChangeGesture();
                         protectParam->setValueNotifyingHost(protectOn ? 0.0f : 1.0f);
                         protectParam->endChangeGesture();
                     });
    }

    // Auto-Panic watchdog.
    if (auto* autoPanicParam = audioProcessor.getValueTreeState().getParameter("AutoPanic"))
    {
        const bool autoPanicOn = autoPanicParam->getValue() > 0.5f;
        menu.addItem("Auto-Panic", true, autoPanicOn,
                     [autoPanicParam, autoPanicOn]()
                     {
                         autoPanicParam->beginChangeGesture();
                         autoPanicParam->setValueNotifyingHost(autoPanicOn ? 0.0f : 1.0f);
                         autoPanicParam->endChangeGesture();
                     });
    }

    menu.addSeparator();

    menu.addItem("Interactive Tour...", true, false,
                 [safeThis]()
                 {
                     if (safeThis != nullptr)
                         safeThis->walkthroughController.start(false);
                 });

    menu.addItem("Help / Shortcuts", true, false,
                 [this]()
                 {
                     helpOverlay.toggle();
                     helpOverlay.setBounds(getLocalBounds());
                     updateHeadViewGLForOverlays();
                 });

    menu.addItem("About Spatial Echo...", true, false, [this]() { showAboutDialog(); });

    // Keep the wordmark lit while its menu is open.
    wordmarkMenuOpen = true;
    repaint(headerTitleBounds);
    menu.showMenuAsync(MenuChrome::popupOptions(brandButton),
                       [safeThis](int)
                       {
                           if (safeThis == nullptr)
                               return;
                           safeThis->interactiveWalkthrough.detachFromModalComponent();
                           safeThis->wordmarkMenuOpen = false;
                           safeThis->walkthroughController.setAppMenuVisible(false);
                           if (safeThis->interactiveWalkthrough.isVisible())
                               safeThis->interactiveWalkthrough.toFront(false);
                           safeThis->repaint(safeThis->headerTitleBounds);
                       });
    interactiveWalkthrough.attachToCurrentPopupMenu();
}

void SpatialEchoAudioProcessorEditor::confirmPresetAction(const juce::String& title, const juce::String& message,
                                                          const juce::String& actionLabel,
                                                          std::function<void(bool)> onResult)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::WarningIcon);
    window->addButton(actionLabel, 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->setLookAndFeel(&getSharedPopupLookAndFeel());
    window->setAlwaysOnTop(true);

    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);
    window->enterModalState(true, juce::ModalCallbackFunction::create(
                                      [safeThis, window, callback = std::move(onResult)](int result) mutable
                                      {
                                          window->setLookAndFeel(nullptr);
                                          const bool confirmed = (result == 1);
                                          if (safeThis != nullptr && callback)
                                              callback(confirmed);
                                          delete window;
                                      }));
}

void SpatialEchoAudioProcessorEditor::showPresetMessage(const juce::String& title, const juce::String& message)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::InfoIcon);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->setLookAndFeel(&getSharedPopupLookAndFeel());
    window->setAlwaysOnTop(true);

    window->enterModalState(true, juce::ModalCallbackFunction::create(
                                      [window](int)
                                      {
                                          window->setLookAndFeel(nullptr);
                                          delete window;
                                      }));
}

void SpatialEchoAudioProcessorEditor::showAboutDialog()
{
    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle = "About Spatial Echo";
    opts.dialogBackgroundColour = CRTLookAndFeel::getVoidBlack();
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = false;
    opts.resizable = false;
    opts.useBottomRightCornerResizer = false;
    opts.componentToCentreAround = this;
    opts.content.setOwned(new AboutDialogContent());
    auto* dialogWindow = opts.launchAsync();
    if (dialogWindow != nullptr)
    {
        dialogWindow->setAlwaysOnTop(true);
        aboutDialog = dialogWindow;     // Store as SafePointer for GL z-order tracking
        updateHeadViewGLForOverlays();  // Detach GL while dialog is open
    }
}

void SpatialEchoAudioProcessorEditor::updateHeadViewGLForOverlays()
{
    // Detach GL context while a covering 2D overlay is visible (help
    // overlay, About dialog, tour). The preset browser is exempt - its panel
    // never overlaps the head view, so the head stays live during audition.
    // Called whenever an overlay's visibility changes; updateGLAttachment()
    // decides attach/detach.
    auto& headView = spatialPanel.getHeadView();

    // The About dialog is a separate modal window, not a child component.
    headView.setAboutDialogOpen(aboutDialog != nullptr);
    headView.updateGLAttachment();
}

void SpatialEchoAudioProcessorEditor::promptForPresetName(const juce::String& title, const juce::String& initialName,
                                                          const juce::String& actionLabel,
                                                          std::function<void(const juce::String&)> onConfirm,
                                                          bool allowEmpty, const juce::String& fieldLabel)
{
    auto* window = new juce::AlertWindow(title, juce::String(), juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", initialName, fieldLabel);
    window->addButton(actionLabel, 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->setLookAndFeel(&getSharedPopupLookAndFeel());
    window->setAlwaysOnTop(true);

    if (auto* editor = window->getTextEditor("name"))
        editor->selectAll();

    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);
    window->enterModalState(true, juce::ModalCallbackFunction::create(
                                      [safeThis, window, confirm = std::move(onConfirm), allowEmpty](int result) mutable
                                      {
                                          window->setLookAndFeel(nullptr);
                                          if (result == 1 && safeThis != nullptr)
                                          {
                                              const auto text = window->getTextEditor("name")->getText().trim();
                                              if (text.isNotEmpty() || allowEmpty)
                                                  confirm(text);
                                              else
                                                  safeThis->showPresetMessage("Invalid Name",
                                                                              "Preset name cannot be empty.");
                                          }
                                          delete window;
                                      }));
}

// ---- Per-head subdivision menu (opened from a delay head readout) ----

void SpatialEchoAudioProcessorEditor::showHeadSubdivisionMenu(int headNumber, bool forWalkthrough)
{
    // Only show menu if tempo sync is enabled
    auto* tempoSyncParam = audioProcessor.getValueTreeState().getRawParameterValue("TempoSync");
    if (!forWalkthrough && (tempoSyncParam == nullptr || *tempoSyncParam < 0.5f))
        return;  // Tempo sync is OFF, don't show menu

    // Determine which parameter to update based on head number
    juce::String parameterID;

    switch (headNumber)
    {
        case 1:
            parameterID = "Head1NoteDivision";
            break;
        case 2:
            parameterID = "Head2NoteDivision";
            break;
        case 3:
            parameterID = "Head3NoteDivision";
            break;
        default:
            return;  // Invalid head number
    }

    // Close any menu already open (e.g. another head's) before showing this one.
    dismissDivisionMenu();

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr)
        return;

    // Create custom grid component for 3-column layout with even gutters
    auto* grid = new DivisionMenuGrid();
    juce::Component::SafePointer<SpatialEchoAudioProcessorEditor> safeThis(this);

    if (auto* divisionParam = audioProcessor.getValueTreeState().getRawParameterValue(parameterID))
        grid->setCurrentIndex(juce::roundToInt(divisionParam->load()));

    // Deleting the host mid-mouse/key callback would free the grid under its
    // own event handler, so every dismissal is deferred via callAsync.
    auto deferredDismiss = [safeThis]()
    {
        juce::MessageManager::callAsync(
            [safeThis]()
            {
                if (safeThis != nullptr)
                    safeThis->dismissDivisionMenu();
            });
    };

    grid->onItemSelected = [safeThis, parameterID, deferredDismiss](int index)
    {
        if (safeThis != nullptr)
            SpatialEchoParameterWrite::setChoiceById(safeThis->audioProcessor.getValueTreeState(), parameterID, index,
                                                     true);
        deferredDismiss();
    };
    grid->onDismissRequested = deferredDismiss;  // ESC (grid holds keyboard focus)

    auto buttonBounds = delayPanel.getHeadValueScreenBounds(headNumber);
    int gridWidth = grid->getPreferredWidth();
    int gridHeight = grid->getPreferredHeight();
    auto placement = SpatialEchoDropdownWindow::calculateConstrainedBounds(
        buttonBounds, {gridWidth, gridHeight}, SpatialEchoDropdownWindow::getDisplayUserAreaFor(buttonBounds),
        topLevel->getScreenBounds());

    grid->setBounds(0, 0, gridWidth, gridHeight);

    auto host = std::make_unique<SpatialEchoDropdownWindow::Host>();
    host->onInputAttempt = deferredDismiss;  // outside click while modal

    if (forWalkthrough)
        host->setModalEventPassthrough(&interactiveWalkthrough);
    divisionMenuWindow = std::move(host);
    divisionMenuWindow->setOpaque(false);
    divisionMenuWindow->addAndMakeVisible(grid);
    divisionMenuWindow->setSize(placement.bounds.getWidth(), placement.bounds.getHeight());
    SpatialEchoDropdownWindow::addToDesktop(*divisionMenuWindow, topLevel);
    SpatialEchoDropdownWindow::setTopLeftPositionForNativeParent(*divisionMenuWindow, placement.bounds, topLevel);
    divisionMenuWindow->setVisible(true);
    SpatialEchoDropdownWindow::setOutsideClickDismissal(*divisionMenuWindow, true);
    divisionMenuWindow->toFront(true);
    grid->grabKeyboardFocus();  // ESC needs real focus; modal state does not deliver keys
    if (forWalkthrough)
        interactiveWalkthrough.attachToCurrentModalComponent();
}

void SpatialEchoAudioProcessorEditor::dismissDivisionMenu()
{
    divisionMenuWindow.reset();  // Host dtor exits modal state and deletes the grid
}
