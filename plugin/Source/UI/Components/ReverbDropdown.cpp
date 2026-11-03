// ReverbDropdown: "REVERB" header and its drawer: engine, presets, and the reverb macros (size, decay, tone, etc.).

#include "ReverbDropdown.h"
#include "../UIConstants.h"
#include "../../Presets/ReverbPresetLibrary.h"
#include "../../Presets/ReverbPresetApplicator.h"
#include "../../ParameterWriteHelpers.h"
#include "CRTLookAndFeel.h"
#include "DropdownWindowUtils.h"
#include <cmath>
#include <utility>

namespace
{
constexpr int kDropdownWidth = 520;
constexpr int kModeRowHeight = 26;
constexpr int kAdvancedGap = 8;
constexpr int kFooterHeight = 96;
constexpr int kColumnGap = 18;
constexpr int kSectionLabelHeight = 16;
constexpr int kParamLabelHeight = 13;
constexpr int kSliderHeight = 22;
constexpr int kRowSpacing = 8;
constexpr int kSectionSpacing = 12;
constexpr int kCustomPresetId = static_cast<int>(kNumReverbPresets) + 1;  // After all real presets
}  // namespace

//==============================================================================
// ReverbDropdown - section header
//==============================================================================

ReverbDropdown::ReverbDropdown(juce::AudioProcessorValueTreeState& apvts)
    : SectionDropdown("REVERB", false)
    , valueTreeState(apvts)
{
    setTooltip("Left-click to open/close. Right-click or use PIN to keep this panel open.");
}

ReverbDropdown::~ReverbDropdown()
{
    hideDropdown();
}

void ReverbDropdown::resyncTypeComboFromPreset()
{
    if (dropdownContent != nullptr && dropdownContent->isShowing())
        dropdownContent->resyncTypeComboFromPreset();
}

std::optional<juce::Rectangle<int>> ReverbDropdown::getWalkthroughScreenBounds(WalkthroughRegion region) const
{
    if (dropdownContent == nullptr || !dropdownContent->isShowing())
        return std::nullopt;

    const auto bounds = dropdownContent->getWalkthroughScreenBounds(region);
    if (bounds.isEmpty())
        return std::nullopt;

    return bounds;
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
bool ReverbDropdown::applyPresetSelectionForTesting(int engine, int presetIndex)
{
    auto content = createConfiguredContent();
    return content->applyPresetSelectionForTesting(engine, presetIndex);
}
#endif

void ReverbDropdown::pinnedStateChanged(bool pinned)
{
    if (dropdownContent != nullptr)
        dropdownContent->setPinnedState(pinned);
}

std::unique_ptr<ReverbDropdown::DropdownContent> ReverbDropdown::createConfiguredContent()
{
    auto content = std::make_unique<DropdownContent>(valueTreeState, lastPresetIndex, lastCloudPresetIndex);
    content->onPresetChanged = [this](int index) { lastPresetIndex = index; };
    content->onCloudPresetChanged = [this](int index) { lastCloudPresetIndex = index; };
    content->onPinToggle = [this]() { togglePinned(); };
    content->setPinnedState(isPinnedOpen());
    return content;
}

void ReverbDropdown::createDropdownWindow()
{
    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr)
        return;

    auto contentOwner = createConfiguredContent();
    auto* content = contentOwner.get();
    dropdownContent = content;

    const int dropdownWidth = content->getPreferredWidth();
    const int dropdownHeight = content->getPreferredHeight();
    content->setBounds(0, 0, dropdownWidth, dropdownHeight);

    // The drawer scrolls when the display is too short for it.
    auto viewport = std::make_unique<juce::Viewport>();
    auto* viewportPtr = viewport.get();
    viewport->setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::all);
    viewport->setScrollBarThickness(10);
    viewport->setViewedComponent(contentOwner.release(), true);

    auto placementFor = [this, dropdownWidth, topLevel](int height)
    {
        const auto anchor = getScreenBounds();
        return SpatialEchoDropdownWindow::calculateCenteredBounds(
            anchor, {dropdownWidth, height}, SpatialEchoDropdownWindow::getDisplayUserAreaFor(anchor),
            topLevel->getScreenBounds());
    };

    const auto placement = placementFor(dropdownHeight);
    viewport->setScrollBarsShown(placement.heightConstrained, false, true, false);
    viewport->setBounds(0, 0, placement.bounds.getWidth(), placement.bounds.getHeight());
    openDropdownWindow(std::move(viewport), placement.bounds);

    // Switching engines changes the drawer's height; re-place the window.
    content->onRequestResize = [this, content, dropdownWidth, viewportPtr, topLevel, placementFor](int newHeight)
    {
        auto* window = getDropdownWindow();
        if (window == nullptr)
            return;

        content->setSize(dropdownWidth, newHeight);
        const auto newPlacement = placementFor(newHeight);
        viewportPtr->setScrollBarsShown(newPlacement.heightConstrained, false, true, false);
        viewportPtr->setBounds(0, 0, newPlacement.bounds.getWidth(), newPlacement.bounds.getHeight());
        SpatialEchoDropdownWindow::setBoundsForNativeParent(*window, newPlacement.bounds, topLevel);
    };
}

//==============================================================================
// DropdownContent - the reverb macro drawer
//==============================================================================

ReverbDropdown::DropdownContent::DropdownContent(juce::AudioProcessorValueTreeState& apvts, int initialPresetIndex,
                                                 int initialCloudPresetIndex)
    : valueTreeState(apvts)
{
    setOpaque(false);

    setupSectionLabel(spaceSectionLabel, "SPACE");
    setupSectionLabel(toneSectionLabel, "TONE");
    setupSectionLabel(textureSectionLabel, "TEXTURE");
    setupSectionLabel(motionSectionLabel, "MOTION");

    // Spring TYPE preset combo with click-through arrows
    for (size_t i = 0; i < kNumReverbPresets; ++i)
        typeCombo.addItem(kReverbPresets[i].name, static_cast<int>(i + 1));
    typeCombo.addItem("Custom", kCustomPresetId);
    typeCombo.setItemEnabled(kCustomPresetId, false);  // Custom is status-only (auto when edited)
    typeCombo.setSelectedItemIndex(juce::jlimit(0, typeCombo.getNumItems() - 1, initialPresetIndex),
                                   juce::dontSendNotification);
    typeCombo.onChange = [this]()
    {
        const int selectedIndex = typeCombo.getSelectedItemIndex();
        if (onPresetChanged)
            onPresetChanged(selectedIndex);
        applySpringPreset(selectedIndex);
    };
    addAndMakeVisible(typeCombo);

    SectionDropdown::configureStepButton(prevButton, "<", [this]() { stepPreset(-1); });
    SectionDropdown::configureStepButton(nextButton, ">", [this]() { stepPreset(+1); });
    addAndMakeVisible(prevButton);
    addAndMakeVisible(nextButton);

    SectionDropdown::configurePinToggle(pinToggleButton,
                                        [this]()
                                        {
                                            if (onPinToggle)
                                                onPinToggle();
                                        });
    addAndMakeVisible(pinToggleButton);

    // Engine selector combo (Spring / Cloud)
    engineCombo.addItem("Spring", 1);
    engineCombo.addItem("Cloud", 2);
    engineCombo.setTooltip("Reverb engine. Spring uses waveguide tank; Cloud uses CloudSeed algorithmic reverb.");
    addAndMakeVisible(engineCombo);

    // Cloud preset combo
    for (size_t i = 0; i < kNumCloudReverbPresets; ++i)
    {
        cloudPresetCombo.addItem(kCloudReverbPresets[i].name, static_cast<int>(i + 1));
    }
    cloudPresetCombo.addItem("Custom", static_cast<int>(kNumCloudReverbPresets + 1));
    cloudPresetCombo.setItemEnabled(static_cast<int>(kNumCloudReverbPresets + 1), false);
    cloudPresetCombo.setSelectedItemIndex(
        juce::jlimit(0, static_cast<int>(kNumCloudReverbPresets), initialCloudPresetIndex), juce::dontSendNotification);
    cloudPresetCombo.setTooltip("Cloud reverb character preset.");
    cloudPresetCombo.onChange = [this]()
    {
        const int selectedIndex = cloudPresetCombo.getSelectedItemIndex();
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(kNumCloudReverbPresets))
            applyCloudPreset(selectedIndex);
        if (onCloudPresetChanged)
            onCloudPresetChanged(selectedIndex);
    };
    addAndMakeVisible(cloudPresetCombo);

    SectionDropdown::configureStepButton(cloudPrevButton, "<", [this]() { stepCloudPreset(-1); });
    SectionDropdown::configureStepButton(cloudNextButton, ">", [this]() { stepCloudPreset(+1); });
    addAndMakeVisible(cloudPrevButton);
    addAndMakeVisible(cloudNextButton);

    // Setup all sliders
    setupSlider(sizeSlider, sizeLabel, "Size", "");
    setupSlider(decaySlider, decayLabel, "Decay", "");
    setupSlider(toneSlider, toneLabel, "Tone", "");
    setupSlider(characterSlider, characterLabel, "Character", "");
    setupSlider(modSlider, modLabel, "Mod", "");
    setupSlider(widthSlider, widthLabel, "Width", "");
    setupSlider(preDelaySlider, preDelayLabel, "Pre-Delay", "");
    setupSlider(duckSlider, duckLabel, "Ducking", "");

    typeCombo.setTooltip("Reverb TYPE preset. Each emulates a different tank or room character.");
    sizeSlider.setTooltip("Reverb size. Larger values create a bigger, more spacious room and a later-blooming tail.");
    decaySlider.setTooltip("Reverb tail length. Higher values sustain the reverb longer.");
    toneSlider.setTooltip("Tone (dark to bright).");
    characterSlider.setTooltip("Per-engine character. Spring: boing/tension. Cloud: bloom/density.");
    modSlider.setTooltip("Modulation depth. Adds motion and chorusing to the tail.");
    widthSlider.setTooltip("Stereo width. 0% is mono; the reverb always folds cleanly to mono.");
    preDelaySlider.setTooltip(juce::String(juce::CharPointer_UTF8(
        "Gap before reverb onset (0\u2013200 ms). Creates separation between the dry signal and reverb.")));
    duckSlider.setTooltip("Dry-input keyed attenuation applied only to the reverb wet branch. Higher values clear "
                          "space around new notes while preserving the tail between phrases.");

    // Attachments
    sizeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(valueTreeState,
                                                                                            "ReverbSize", sizeSlider);
    decayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbDecay", decaySlider);
    toneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(valueTreeState,
                                                                                            "ReverbTone", toneSlider);
    characterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbCharacter", characterSlider);
    modAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(valueTreeState, "ReverbMod", modSlider);
    widthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbWidth", widthSlider);
    preDelayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbPreDelay", preDelaySlider);
    duckAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbDuckAmount", duckSlider);
    engineAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        valueTreeState, "ReverbEngine", engineCombo);

    // Reset button
    resetButton.setButtonText("Reset Defaults");
    resetButton.setColour(juce::TextButton::textColourOffId, CRTLookAndFeel::getTerminalGreen());
    resetButton.setColour(juce::TextButton::buttonColourId, CRTLookAndFeel::getVoidBlack().withAlpha(0.15f));
    resetButton.setTooltip("Reset all reverb parameters to factory defaults and select the first preset.");
    resetButton.onClick = [this]() { resetToDefaults(); };
    addAndMakeVisible(resetButton);

    engineValue = valueTreeState.getRawParameterValue("ReverbEngine");
    lastEngineIndex = -1;  // Force first syncEngineVisibility() to run unconditionally
    syncEngineVisibility();

    // Resync TYPE combo to the current reverb state after dropdown opens.
    // This handles presets loaded while the dropdown was closed.
    resyncTypeComboFromPreset();

    startTimerHz(20);

    // Add slider listeners to detect manual edits
    sizeSlider.addListener(this);
    decaySlider.addListener(this);
    toneSlider.addListener(this);
    characterSlider.addListener(this);
    modSlider.addListener(this);
    widthSlider.addListener(this);
    preDelaySlider.addListener(this);
    duckSlider.addListener(this);
}

ReverbDropdown::DropdownContent::~DropdownContent()
{
    sizeSlider.removeListener(this);
    decaySlider.removeListener(this);
    toneSlider.removeListener(this);
    characterSlider.removeListener(this);
    modSlider.removeListener(this);
    widthSlider.removeListener(this);
    preDelaySlider.removeListener(this);
    duckSlider.removeListener(this);
    stopTimer();
}

void ReverbDropdown::DropdownContent::stepPreset(int delta)
{
    const int current = typeCombo.getSelectedItemIndex();
    const int count = typeCombo.getNumItems();
    if (count <= 1)
        return;
    // Skip the "Custom" entry (last item)
    const int navigableCount = count - 1;
    int next = ((current < navigableCount ? current : 0) + delta + navigableCount) % navigableCount;
    typeCombo.setSelectedItemIndex(next, juce::sendNotificationSync);
}

void ReverbDropdown::DropdownContent::sliderValueChanged(juce::Slider* slider)
{
    juce::ignoreUnused(slider);
    // If a slider is changed manually (not during preset apply), switch to Custom
    if (!presetApplyInProgress)
        switchToCustomPreset();
}

void ReverbDropdown::DropdownContent::switchToCustomPreset()
{
    const bool isSpring = (engineValue == nullptr || engineValue->load() < 0.5f);
    if (isSpring)
    {
        const int customIndex = typeCombo.getNumItems() - 1;
        if (typeCombo.getSelectedItemIndex() != customIndex)
        {
            typeCombo.setSelectedItemIndex(customIndex, juce::dontSendNotification);
            if (onPresetChanged)
                onPresetChanged(customIndex);
        }
    }
    else
    {
        const int customIndex = cloudPresetCombo.getNumItems() - 1;
        if (cloudPresetCombo.getSelectedItemIndex() != customIndex)
        {
            cloudPresetCombo.setSelectedItemIndex(customIndex, juce::dontSendNotification);
            if (onCloudPresetChanged)
                onCloudPresetChanged(customIndex);
        }
    }
}

void ReverbDropdown::DropdownContent::resyncTypeComboFromPreset()
{
    // After a global preset load, select the TYPE whose stored macro values match
    // the current state; otherwise show the disabled "Custom" entry.
    auto current = [this](const char* id) { return valueTreeState.getRawParameterValue(id)->load(); };
    const float vals[7] = {current("ReverbSize"), current("ReverbDecay"),     current("ReverbTone"),
                           current("ReverbMod"),  current("ReverbCharacter"), current("ReverbPreDelay"),
                           current("ReverbWidth")};

    static constexpr float kTol = 0.005f;  // absorbs NormalisableRange quantisation
    auto matches = [&vals](const auto& p)
    {
        const float ref[7] = {p.size, p.decay, p.tone, p.mod, p.character, p.preDelay, p.width};
        for (int i = 0; i < 7; ++i)
            if (std::abs(vals[i] - ref[i]) >= kTol)
                return false;
        return true;
    };

    const bool isSpring = (engineValue == nullptr || engineValue->load() < 0.5f);
    int matched = -1;
    if (isSpring)
    {
        for (size_t i = 0; i < kReverbPresets.size(); ++i)
            if (matches(kReverbPresets[i]))
            {
                matched = (int)i;
                break;
            }
    }
    else
    {
        for (size_t i = 0; i < kCloudReverbPresets.size(); ++i)
            if (matches(kCloudReverbPresets[i]))
            {
                matched = (int)i;
                break;
            }
    }

    presetApplyInProgress = true;  // selection mirrors state; must not re-apply or dirty
    auto& combo = isSpring ? typeCombo : cloudPresetCombo;
    if (matched >= 0)
        combo.setSelectedItemIndex(matched, juce::dontSendNotification);
    else
        switchToCustomPreset();
    presetApplyInProgress = false;
}

void ReverbDropdown::DropdownContent::setupSectionLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setColour(juce::Label::textColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.7f));
    label.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

void ReverbDropdown::DropdownContent::setupParamLabel(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredLeft);
    label.setColour(juce::Label::textColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.8f));
    label.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeMicroSize));
    label.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(label);
}

void ReverbDropdown::DropdownContent::setupSlider(ParameterValueSlider& slider, juce::Label& label,
                                                  const juce::String& text, const juce::String& suffix)
{
    setupParamLabel(label, text);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
    slider.setColour(juce::Slider::textBoxTextColourId, CRTLookAndFeel::getTerminalGreen());
    slider.setColour(juce::Slider::textBoxOutlineColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.3f));
    slider.setColour(juce::Slider::textBoxBackgroundColourId, CRTLookAndFeel::getVoidBlack().withAlpha(0.65f));
    slider.setColour(juce::Slider::trackColourId, CRTLookAndFeel::getTerminalGreen().withAlpha(0.25f));
    slider.setColour(juce::Slider::thumbColourId, CRTLookAndFeel::getTerminalGreen());
    slider.setTextValueSuffix(suffix);

    addAndMakeVisible(slider);
}

void ReverbDropdown::DropdownContent::timerCallback()
{
    syncEngineVisibility();
}

void ReverbDropdown::DropdownContent::syncEngineVisibility()
{
    const int currentEngine = (engineValue != nullptr) ? static_cast<int>(engineValue->load()) : 0;
    if (currentEngine == lastEngineIndex)
        return;

    lastEngineIndex = currentEngine;
    const bool isSpring = (currentEngine == 0);

    // Spring preset controls
    typeCombo.setVisible(isSpring);
    prevButton.setVisible(isSpring);
    nextButton.setVisible(isSpring);

    // Cloud preset controls
    cloudPresetCombo.setVisible(!isSpring);
    cloudPrevButton.setVisible(!isSpring);
    cloudNextButton.setVisible(!isSpring);

    // Context-sensitive labels for shared parameters
    sizeLabel.setText(isSpring ? "Tank Size" : "Room Size", juce::dontSendNotification);
    sizeSlider.setTooltip(isSpring
                              ? "Spring tank size. Larger values give a bigger, more spacious spring response with "
                                "more late tail."
                              : "Virtual room size. Larger values produce longer, more spacious reflections.");

    resized();
    if (onRequestResize)
        onRequestResize(getPreferredHeight());
    repaint();
}

void ReverbDropdown::DropdownContent::stepCloudPreset(int delta)
{
    const int customIndex = static_cast<int>(kNumCloudReverbPresets);
    int current = cloudPresetCombo.getSelectedItemIndex();
    if (current >= customIndex)
        current = 0;
    int next = current + delta;
    if (next < 0)
        next = static_cast<int>(kNumCloudReverbPresets) - 1;
    if (next >= static_cast<int>(kNumCloudReverbPresets))
        next = 0;
    cloudPresetCombo.setSelectedItemIndex(next, juce::sendNotificationSync);
}

void ReverbDropdown::DropdownContent::applyCloudPreset(int presetIndex)
{
    if (presetIndex >= 0 && presetIndex < static_cast<int>(kNumCloudReverbPresets))
    {
        presetApplyInProgress = true;
        // Presets store UI-space macro values directly.
        const auto& p = kCloudReverbPresets[static_cast<size_t>(presetIndex)];

        auto setParam = [this](const char* id, float value)
        { SpatialEchoParameterWrite::setFloatById(valueTreeState, id, value, true); };

        setParam("ReverbSize", p.size);
        setParam("ReverbDecay", p.decay);
        setParam("ReverbTone", p.tone);
        setParam("ReverbMod", p.mod);
        setParam("ReverbCharacter", p.character);
        setParam("ReverbPreDelay", p.preDelay);
        setParam("ReverbWidth", p.width);

        presetApplyInProgress = false;
    }
}

void ReverbDropdown::DropdownContent::applySpringPreset(int presetIndex)
{
    if (presetIndex >= 0 && presetIndex < static_cast<int>(kNumReverbPresets))
    {
        presetApplyInProgress = true;  // Suppress Custom switch during preset apply
        SpatialEchoReverbPresets::applySpringPresetToState(valueTreeState, presetIndex, true);

        presetApplyInProgress = false;
    }
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
bool ReverbDropdown::DropdownContent::applyPresetSelectionForTesting(int engine, int presetIndex)
{
    if (engine == 0 && presetIndex >= 0 && presetIndex < static_cast<int>(kNumReverbPresets))
    {
        typeCombo.setSelectedItemIndex(presetIndex, juce::dontSendNotification);
        if (typeCombo.onChange)
            typeCombo.onChange();
        return true;
    }
    if (engine == 1 && presetIndex >= 0 && presetIndex < static_cast<int>(kNumCloudReverbPresets))
    {
        cloudPresetCombo.setSelectedItemIndex(presetIndex, juce::dontSendNotification);
        if (cloudPresetCombo.onChange)
            cloudPresetCombo.onChange();
        return true;
    }
    return false;
}
#endif

void ReverbDropdown::DropdownContent::resetToDefaults()
{
    SpatialEchoParameterWrite::setChoiceById(valueTreeState, "ReverbEngine", 0, true);
    engineCombo.setSelectedItemIndex(0, juce::dontSendNotification);
    typeCombo.setSelectedItemIndex(0, juce::dontSendNotification);
    applySpringPreset(0);
    SpatialEchoParameterWrite::setChoiceById(valueTreeState, "ReverbRouting", 0, true);
    SpatialEchoParameterWrite::setFloatById(valueTreeState, "ReverbDuckAmount", 0.0f, true);
    if (onPresetChanged)
        onPresetChanged(0);
}

void ReverbDropdown::DropdownContent::paint(juce::Graphics& g)
{
    const bool isCloud = (engineValue != nullptr && engineValue->load() > 0.5f);
    SectionDropdown::paintDrawerChrome(g, getLocalBounds().toFloat(), isCloud ? "CLOUD REVERB" : "SPRING REVERB");

    // Column divider
    {
        auto contentBounds = getLocalBounds().reduced(kDrawerPadding, kDrawerPadding);
        contentBounds.removeFromTop(kDrawerHeaderHeight + kDrawerHeaderGap + kPresetRowHeight + kAdvancedGap +
                                    kModeRowHeight + kAdvancedGap);
        contentBounds.removeFromBottom(kFooterHeight);
        const float dividerX = static_cast<float>(contentBounds.getX() + contentBounds.getWidth() / 2);
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.12f));
        g.drawLine(dividerX, static_cast<float>(contentBounds.getY()), dividerX,
                   static_cast<float>(contentBounds.getBottom()), 1.0f);
    }
}

void ReverbDropdown::DropdownContent::resized()
{
    auto bounds = getLocalBounds().reduced(kDrawerPadding, kDrawerPadding);
    pinToggleButton.setBounds(SectionDropdown::pinToggleBounds(getLocalBounds()));

    bounds.removeFromTop(kDrawerHeaderHeight + kDrawerHeaderGap);

    // Engine selector row
    auto engineRow = bounds.removeFromTop(kModeRowHeight);
    {
        const int engineComboWidth = juce::jmax(engineRow.getWidth() / 2, engineCombo.getRequiredValueWidth());
        const int engineX = (engineRow.getWidth() - engineComboWidth) / 2;
        engineCombo.setBounds(engineRow.getX() + engineX, engineRow.getY(), engineComboWidth, engineRow.getHeight());
    }
    bounds.removeFromTop(kAdvancedGap);

    const bool isSpring = (engineValue == nullptr || engineValue->load() < 0.5f);

    // Preset row (Spring or Cloud)
    auto presetRow = bounds.removeFromTop(kPresetRowHeight);
    if (isSpring)
    {
        prevButton.setBounds(presetRow.removeFromLeft(kStepButtonWidth));
        nextButton.setBounds(presetRow.removeFromRight(kStepButtonWidth));
        typeCombo.setBounds(presetRow.withWidth(juce::jmax(presetRow.getWidth(), typeCombo.getRequiredValueWidth())));
    }
    else
    {
        cloudPrevButton.setBounds(presetRow.removeFromLeft(kStepButtonWidth));
        cloudNextButton.setBounds(presetRow.removeFromRight(kStepButtonWidth));
        cloudPresetCombo.setBounds(
            presetRow.withWidth(juce::jmax(presetRow.getWidth(), cloudPresetCombo.getRequiredValueWidth())));
    }

    bounds.removeFromTop(kAdvancedGap);

    auto footerArea = bounds.removeFromBottom(kFooterHeight);

    const int columnWidth = (bounds.getWidth() - kColumnGap) / 2;
    auto leftColumn = bounds.removeFromLeft(columnWidth);
    bounds.removeFromLeft(kColumnGap);
    auto rightColumn = bounds;

    auto layoutSection = [](juce::Rectangle<int> area, juce::Label& sectionLabel,
                            std::initializer_list<std::pair<juce::Label*, juce::Slider*>> items)
    {
        sectionLabel.setBounds(area.removeFromTop(kSectionLabelHeight));
        area.removeFromTop(4);
        for (auto& item : items)
        {
            auto row = area.removeFromTop(kParamLabelHeight + kSliderHeight + kRowSpacing);
            item.first->setBounds(row.removeFromTop(kParamLabelHeight));
            item.second->setBounds(row.removeFromTop(kSliderHeight));
        }
        return area;
    };

    // Unified layout for both engines (full macro schema)
    leftColumn = layoutSection(leftColumn, spaceSectionLabel,
                               {
                                   {&sizeLabel, &sizeSlider},
                                   {&decayLabel, &decaySlider},
                                   {&preDelayLabel, &preDelaySlider},
                                   {&widthLabel, &widthSlider},
                               });

    rightColumn = layoutSection(rightColumn, toneSectionLabel,
                                {
                                    {&toneLabel, &toneSlider},
                                    {&characterLabel, &characterSlider},
                                });
    rightColumn.removeFromTop(kSectionSpacing);
    rightColumn = layoutSection(rightColumn, motionSectionLabel,
                                {
                                    {&modLabel, &modSlider},
                                });
    rightColumn.removeFromTop(kSectionSpacing);
    layoutSection(rightColumn, textureSectionLabel, {{&duckLabel, &duckSlider}});

    const int resetButtonHeight = 30;
    const int resetY = (footerArea.getHeight() - resetButtonHeight) / 2;
    resetButton.setBounds(footerArea.withTrimmedTop(resetY).removeFromTop(resetButtonHeight).reduced(28, 0));
}

int ReverbDropdown::DropdownContent::getPreferredWidth() const
{
    return kDropdownWidth;
}

int ReverbDropdown::DropdownContent::getPreferredHeight() const
{
    // Engine row + gap added before preset row
    const int base = kDrawerPadding * 2 + kDrawerHeaderHeight + kDrawerHeaderGap + kModeRowHeight +
                     kAdvancedGap  // engine combo row
                     + kPresetRowHeight + kAdvancedGap;

    auto sectionHeight = [](int itemCount)
    { return kSectionLabelHeight + 4 + itemCount * (kParamLabelHeight + kSliderHeight + kRowSpacing); };

    const int leftColumn = sectionHeight(4);
    const int rightColumn = sectionHeight(2) + kSectionSpacing + sectionHeight(1) + kSectionSpacing + sectionHeight(1);
    const int columns = juce::jmax(leftColumn, rightColumn);

    return base + kModeRowHeight + kAdvancedGap + columns + kFooterHeight;
}

juce::Rectangle<int> ReverbDropdown::DropdownContent::getWalkthroughScreenBounds(WalkthroughRegion region) const
{
    auto unionBounds = [](std::initializer_list<const juce::Component*> components)
    {
        juce::Rectangle<int> result;
        for (const auto* component : components)
        {
            if (component == nullptr || !component->isVisible())
                continue;

            const auto bounds = component->getScreenBounds();
            result = result.isEmpty() ? bounds : result.getUnion(bounds);
        }
        return result;
    };

    switch (region)
    {
        case WalkthroughRegion::engine:
            return engineCombo.getScreenBounds();

        case WalkthroughRegion::preset:
            if (typeCombo.isVisible())
                return unionBounds({&prevButton, &typeCombo, &nextButton});
            return unionBounds({&cloudPrevButton, &cloudPresetCombo, &cloudNextButton});

        case WalkthroughRegion::preDelayWidth:
            return unionBounds({&preDelayLabel, &preDelaySlider, &widthLabel, &widthSlider});

        case WalkthroughRegion::character:
            return unionBounds({&characterLabel, &characterSlider});

        case WalkthroughRegion::mod:
            return unionBounds({&modLabel, &modSlider});

        case WalkthroughRegion::ducking:
            return unionBounds({&duckLabel, &duckSlider});

        case WalkthroughRegion::pin:
            return pinToggleButton.getScreenBounds();

        case WalkthroughRegion::reset:
            return resetButton.getScreenBounds();
    }

    return {};
}

void ReverbDropdown::DropdownContent::setPinnedState(bool pinned)
{
    SectionDropdown::showPinnedState(pinToggleButton, pinned);
}
