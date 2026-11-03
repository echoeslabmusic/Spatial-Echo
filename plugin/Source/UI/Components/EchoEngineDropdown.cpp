// EchoEngineDropdown: "ECHO ENGINE" header with the head ratio drawer and the engine status light.

#include "EchoEngineDropdown.h"
#include "../UIConstants.h"
#include "CRTLookAndFeel.h"
#include "DropdownWindowUtils.h"
#include "../../PluginProcessor.h"

namespace
{
constexpr int kDropdownWidth = 280;
constexpr int kCaptionRowHeight = 24;  // 24px minimum (section caption strips)
constexpr int kCaptionGap = 4;
// Header status light: heat thresholds on the pre-saturation drive envelope,
// tuned by ear (saturator knee ~1.0, gross distortion ~2.4).
constexpr float kHeatAmberOnset = 1.0f;
constexpr float kHeatRedOnset = 2.4f;
constexpr int kAutoPanicFlashTicks = 24;  // ~800 ms blink at 30 Hz
}  // namespace

//==============================================================================
// EchoEngineDropdown - section header
//==============================================================================

EchoEngineDropdown::EchoEngineDropdown(SpatialEchoAudioProcessor& processor, juce::AudioProcessorValueTreeState& apvts)
    : SectionDropdown("ECHO ENGINE", true)  // the status light ticks with the drawer closed
    , audioProcessor(processor)
    , valueTreeState(apvts)
{
    setTooltip("Left-click to open/close. Right-click or use PIN to keep this panel open. "
               "Head ratio lives here; the label doubles as the engine status light.");

    powerParam = valueTreeState.getRawParameterValue("Power");
    lastAutoPanicEvents = audioProcessor.getAutoPanicEventCount();
    lastHeaderColour = CRTLookAndFeel::getTerminalGreen();
}

EchoEngineDropdown::~EchoEngineDropdown()
{
    hideDropdown();
}

juce::Rectangle<int> EchoEngineDropdown::getWalkthroughScreenBounds() const
{
    return getWalkthroughScreenBounds(WalkthroughRegion::drawer);
}

juce::Rectangle<int> EchoEngineDropdown::getWalkthroughScreenBounds(WalkthroughRegion region) const
{
    auto* window = getDropdownWindow();
    if (window == nullptr || !window->isShowing())
        return getScreenBounds();

    if (region == WalkthroughRegion::drawer)
        return window->getScreenBounds();

    if (contentPtr != nullptr)
        return contentPtr->getWalkthroughScreenBounds(region);

    return window->getScreenBounds();
}

// Header status light: heat tints the label green -> amber -> red; Power off
// pulses it red so a silent plug-in is never a mystery; an auto-panic trigger
// blinks it. Runs every tick, drawer open or closed.
void EchoEngineDropdown::timerTick()
{
    ++statusTick;

    const float drive = audioProcessor.getSelfOscillationHeat();
    const float heat01 = juce::jlimit(0.0f, 1.0f, (drive - kHeatAmberOnset) / (kHeatRedOnset - kHeatAmberOnset));

    const juce::uint32 events = audioProcessor.getAutoPanicEventCount();
    if (events != lastAutoPanicEvents)
    {
        lastAutoPanicEvents = events;
        autoPanicFlashTicks = kAutoPanicFlashTicks;
    }

    const bool powerOff = (powerParam != nullptr) && (powerParam->load() < 0.5f);

    const auto green = CRTLookAndFeel::getTerminalGreen();
    const auto amber = CRTLookAndFeel::getWarningAmber();
    const auto red = CRTLookAndFeel::getAlertRed();

    juce::Colour target = green;
    if (autoPanicFlashTicks > 0)
    {
        --autoPanicFlashTicks;
        target = ((autoPanicFlashTicks / 4) & 1) ? green : red;  // ~4 Hz blink
    }
    else if (powerOff)
    {
        const float pulse = 0.55f + 0.35f * std::sin(static_cast<float>(statusTick) * 0.35f);
        target = red.withAlpha(pulse);
    }
    else if (heat01 > 0.0f)
    {
        target = (heat01 <= 0.5f) ? green.interpolatedWith(amber, heat01 * 2.0f)
                                  : amber.interpolatedWith(red, (heat01 - 0.5f) * 2.0f);
    }

    if (target != lastHeaderColour)
    {
        lastHeaderColour = target;
        setTitleColour(target);
    }
}

void EchoEngineDropdown::pinnedStateChanged(bool pinned)
{
    if (contentPtr != nullptr)
        contentPtr->setPinnedState(pinned);
}

void EchoEngineDropdown::createDropdownWindow()
{
    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr)
        return;

    auto content = std::make_unique<DropdownContent>(valueTreeState);
    contentPtr = content.get();
    content->onPinToggle = [this]() { togglePinned(); };
    content->setPinnedState(isPinnedOpen());

    const auto buttonBounds = getScreenBounds();
    const int dropdownWidth = content->getPreferredWidth();
    const int dropdownHeight = content->getPreferredHeight();
    const auto placement = SpatialEchoDropdownWindow::calculateConstrainedBounds(
        buttonBounds, {dropdownWidth, dropdownHeight}, SpatialEchoDropdownWindow::getDisplayUserAreaFor(buttonBounds),
        topLevel->getScreenBounds());

    content->setBounds(0, 0, dropdownWidth, dropdownHeight);
    openDropdownWindow(std::move(content), placement.bounds);
}

//==============================================================================
// DropdownContent - the head ratio drawer
//==============================================================================

EchoEngineDropdown::DropdownContent::DropdownContent(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    setOpaque(false);

    presetCombo.addItem("Classic (1:1.9:2.75)", 1);
    presetCombo.addItem("Idealized (1:2:3)", 2);
    presetCombo.addItem("Golden Ratio (1:1.62:2.62)", 3);
    presetCombo.addItem("Linear (1:1.5:2)", 4);
    presetCombo.setTooltip("Head spacing ratio preset. Defines the timing relationship between playback heads.");
    addAndMakeVisible(presetCombo);

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

    // Create attachments LAST
    presetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        valueTreeState, "HeadRatioPreset", presetCombo);
}

EchoEngineDropdown::DropdownContent::~DropdownContent() = default;

void EchoEngineDropdown::DropdownContent::stepPreset(int delta)
{
    const int current = presetCombo.getSelectedItemIndex();
    const int count = presetCombo.getNumItems();
    if (count <= 0)
        return;
    const int next = (current + delta + count) % count;
    presetCombo.setSelectedItemIndex(next, juce::sendNotificationSync);
}

void EchoEngineDropdown::DropdownContent::paint(juce::Graphics& g)
{
    SectionDropdown::paintDrawerChrome(g, getLocalBounds().toFloat(), "ECHO ENGINE");

    // Section caption (rect laid out in resized()).
    if (!headRatioCaptionBounds.isEmpty())
    {
        g.setColour(CRTLookAndFeel::getTerminalGreen().withAlpha(0.65f));
        g.setFont(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeLabelSize));
        g.drawText("HEAD RATIO", headRatioCaptionBounds, juce::Justification::centredLeft);
    }
}

void EchoEngineDropdown::DropdownContent::resized()
{
    pinToggleButton.setBounds(SectionDropdown::pinToggleBounds(getLocalBounds()));

    auto bounds = getLocalBounds().reduced(kDrawerPadding, kDrawerPadding);
    bounds.removeFromTop(kDrawerHeaderHeight + kDrawerHeaderGap);

    // Captioned head-ratio selector.
    headRatioCaptionBounds = bounds.removeFromTop(kCaptionRowHeight).reduced(kStepButtonWidth, 0);
    bounds.removeFromTop(kCaptionGap);
    auto presetRow = bounds.removeFromTop(kPresetRowHeight);
    prevButton.setBounds(presetRow.removeFromLeft(kStepButtonWidth));
    nextButton.setBounds(presetRow.removeFromRight(kStepButtonWidth));
    presetCombo.setBounds(presetRow);
}

void EchoEngineDropdown::DropdownContent::setPinnedState(bool pinned)
{
    SectionDropdown::showPinnedState(pinToggleButton, pinned);
}

juce::Rectangle<int> EchoEngineDropdown::DropdownContent::getWalkthroughScreenBounds(WalkthroughRegion region) const
{
    switch (region)
    {
        case WalkthroughRegion::drawer:
            return getScreenBounds();
        case WalkthroughRegion::headRatio:
            return localAreaToGlobal(headRatioCaptionBounds)
                .getUnion(prevButton.getScreenBounds())
                .getUnion(presetCombo.getScreenBounds())
                .getUnion(nextButton.getScreenBounds());
    }

    jassertfalse;
    return getScreenBounds();
}

int EchoEngineDropdown::DropdownContent::getPreferredWidth() const
{
    // Content-sized: the widest head-ratio name sits
    // between both step arrows without truncation or overlap.
    return juce::jmax(kDropdownWidth, kDrawerPadding * 2 + kStepButtonWidth * 2 + presetCombo.getRequiredValueWidth());
}

int EchoEngineDropdown::DropdownContent::getPreferredHeight() const
{
    return kDrawerPadding * 2 + kDrawerHeaderHeight + kDrawerHeaderGap + kCaptionRowHeight + kCaptionGap +
           kPresetRowHeight;  // HEAD RATIO
}
