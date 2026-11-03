// DelayPanel: UI panel containing delay mode, repeat rate, intensity, echo level, and head displays.

#include "DelayPanel.h"
#include "DelayHeadActivation.h"
#include "../UIConstants.h"
#include "../../DSP/HeadRatioConstants.h"
#include "../../PluginProcessor.h"
#include <array>
#include <cmath>
#include <utility>

namespace
{
// Logarithmic fill mapping: maps a value to [0, 1] range using log2 scale
// Used for both sync mode (beat fractions) and free mode (millisecond delays)
float logFill(float value, float minValue, float maxValue)
{
    // Clamp to valid range to avoid log of zero or negative
    value = juce::jlimit(minValue, maxValue, value);
    // Map log space: (log2(value) - log2(min)) / (log2(max) - log2(min))
    float fill = (std::log2(value) - std::log2(minValue)) / (std::log2(maxValue) - std::log2(minValue));
    // Clamp to [0.06, 1.0]: 0.06 floor keeps shortest visible as sliver, 1.0 cap is absolute max
    return juce::jlimit(0.06f, 1.0f, fill);
}

// RATE tooltip: used in both constructor and updateEnabledStates
constexpr const char* kRateFreeModeTip = "Base delay time in milliseconds. In free mode, shows 175 to 70 ms. Disabled "
                                         "when SYNC is on; use H1/H2/H3 division menus.";

// Head 2 and 3 spacing relative to head 1 for each HeadRatioPreset choice.
std::pair<float, float> headRatiosForPreset(int preset)
{
    switch (preset)
    {
        case 1:
            return {HeadRatios::IDEALIZED_RATIO_2, HeadRatios::IDEALIZED_RATIO_3};
        case 2:
            return {HeadRatios::GOLDEN_RATIO_2, HeadRatios::GOLDEN_RATIO_3};
        case 3:
            return {HeadRatios::LINEAR_RATIO_2, HeadRatios::LINEAR_RATIO_3};
        default:
            return {HeadRatios::CLASSIC_RATIO_2, HeadRatios::CLASSIC_RATIO_3};
    }
}

// Compact view of the DelaySetting parameter's own choice strings -
// derived rather than duplicated so the UI cannot drift from the
// parameter text the host sees (PluginProcessor "DelaySetting" choices).
juce::String compactDelayModeName(const juce::String& parameterText)
{
    return parameterText.toUpperCase()
        .replace("ALL HEADS", "ALL")
        .replace("ECHO+REVERB", "E+REV")
        .replace("REVERB ONLY", "REV ONLY");
}
}  // namespace

DelayPanel::DelayPanel(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    modeBlock.setLabelText("Mode");
    modeBlock.getSlider().setTooltip("Delay head combination. Selects which tape heads are active.");
    modeBlock.getSlider().setTextBoxStyle(juce::Slider::TextBoxBelow, true, 108, UIConstants::textBoxHeight);
    modeBlock.getSlider().setNumDecimalPlacesToDisplay(0);
    addAndMakeVisible(modeBlock);

    repeatRateBlock.setLabelText("Rate");
    repeatRateBlock.getSlider().setTooltip(kRateFreeModeTip);
    addAndMakeVisible(repeatRateBlock);

    intensityBlock.setLabelText("Feedback");
    intensityBlock.getSlider().setTooltip(
        "Feedback amount. Higher values produce longer repeats; above 95% causes self-oscillation.");
    intensityBlock.getSlider().setWantsKeyboardFocus(true);
    addAndMakeVisible(intensityBlock);

    echoLevelBlock.setLabelText("Echo Level");
    echoLevelBlock.setTitleMetrics(10, UIConstants::typeMicroSize);
    echoLevelBlock.getSlider().setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, UIConstants::textBoxHeight);
    echoLevelBlock.getSlider().setNumDecimalPlacesToDisplay(1);
    constexpr float kHalfSweep = juce::MathConstants<float>::twoPi / 3.0f;  // 12 o'clock center, +/-120 degrees
    // Angles offset by 2*pi: same geometry, but keeps both radians
    // non-negative as setRotaryParameters' contract requires.
    echoLevelBlock.getSlider().setRotaryParameters(juce::MathConstants<float>::twoPi - kHalfSweep,
                                                   juce::MathConstants<float>::twoPi + kHalfSweep, true);
    echoLevelBlock.getSlider().setTooltip("Echo branch level trim (-12 dB to +12 dB).");
    addAndMakeVisible(echoLevelBlock);

    dubLamp.setTooltip("Dub mode. While it reads INPUT OFF, new audio is kept out of the delay "
                       "and the repeats already on tape fade out. Click again to feed input.");
    dubLamp.setTitle("Dub Mode");
    dubLamp.setDescription("When on, mutes new input while existing repeats continue circulating");
    addAndMakeVisible(dubLamp);

    // Delay-behaviour toggles, side by side under the Mode knob.
    pingPongButton.setButtonText("PING-PONG");
    pingPongButton.setTooltip("Ping-pong. Alternates the repeats between the left and right channels.");
    pingPongButton.setWantsKeyboardFocus(true);
    pingPongButton.setTitle("Ping-Pong");
    addAndMakeVisible(pingPongButton);

    tempoSyncButton.setButtonText("SYNC");
    tempoSyncButton.setTooltip("Sync delay timing to the host tempo. Click the H1/H2/H3 values to set note divisions.");
    tempoSyncButton.setWantsKeyboardFocus(true);
    tempoSyncButton.setTitle("Tempo Sync");
    addAndMakeVisible(tempoSyncButton);

    // Head readouts: title label over a clickable value button that opens the
    // head's subdivision menu while SYNC is on.
    for (int head = 0; head < kNumHeads; ++head)
    {
        const auto index = static_cast<size_t>(head);
        const juce::String headName = "H" + juce::String(head + 1);
        auto& title = headTitleLabels[index];
        title.setText(headName, juce::dontSendNotification);
        title.setJustificationType(juce::Justification::centred);
        title.setName("head" + juce::String(head + 1) + "TitleLabel");
        addAndMakeVisible(title);

        auto& valueButton = headValueButtons[index];
        valueButton.setButtonText("-- ms");
        valueButton.setTooltip("Head " + juce::String(head + 1) +
                               " delay time. Click to choose note division when SYNC is on.");
        valueButton.setWantsKeyboardFocus(true);
        valueButton.setName("head" + juce::String(head + 1) + "ValueButton");
        valueButton.onClick = [this, head]()
        {
            auto* tempoSyncParam = valueTreeState.getParameter("TempoSync");
            const bool syncOn = tempoSyncParam != nullptr && tempoSyncParam->getValue() > 0.5f;
            if (syncOn && onHeadClicked)
                onHeadClicked(head + 1);
        };
        addAndMakeVisible(valueButton);
    }

    // Explicit focus order (Accessibility: WCAG 2.1.1 keyboard access)
    modeBlock.getSlider().setExplicitFocusOrder(1);
    repeatRateBlock.getSlider().setExplicitFocusOrder(2);
    intensityBlock.getSlider().setExplicitFocusOrder(3);
    pingPongButton.setExplicitFocusOrder(4);
    tempoSyncButton.setExplicitFocusOrder(5);
    echoLevelBlock.getSlider().setExplicitFocusOrder(6);
    dubLamp.setExplicitFocusOrder(7);
    for (int head = 0; head < kNumHeads; ++head)
        headValueButtons[static_cast<size_t>(head)].setExplicitFocusOrder(8 + head);

    // Component names identify controls in layout diagnostics.
    modeBlock.setSliderName("modeSlider");
    repeatRateBlock.setSliderName("repeatRateSlider");
    intensityBlock.setSliderName("intensitySlider");
    echoLevelBlock.setSliderName("echoLevelSlider");
    pingPongButton.setName("pingPongButton");
    tempoSyncButton.setName("tempoSyncButton");
    dubLamp.setName("dubLamp");

    // Create attachments LAST (after all component setup)
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "DelaySetting", modeBlock.getSlider());

    // Compact mode names for the UI text box, derived from the parameter's
    // own choice strings. SliderParameterAttachment installs its own
    // textFromValueFunction (full parameter text) at construction, so these
    // must be installed AFTER the attachment, followed by updateText() so
    // the initial display renders through the same formatter as every later
    // update (skipping updateText() leaves the first paint showing the full
    // parameter text while reloads show the compact form).
    auto* modeParam = valueTreeState.getParameter("DelaySetting");
    auto modeTextForIndex = [modeParam](int index) -> juce::String
    {
        if (modeParam == nullptr)
            return {};
        return modeParam->getText(modeParam->convertTo0to1(static_cast<float>(index)), 64);
    };
    modeBlock.getSlider().setValueTextFormatter(
        [modeTextForIndex](double value)
        {
            const int index = juce::roundToInt(static_cast<float>(value));
            auto text = modeTextForIndex(index);
            return text.isNotEmpty() ? compactDelayModeName(text) : juce::String(index);
        },
        [this, modeTextForIndex](const juce::String& text)
        {
            const auto trimmed = text.trim();
            if (trimmed.containsOnly("0123456789"))
            {
                const int numeric = trimmed.getIntValue();
                if (numeric >= 1 && numeric <= 12)
                    return static_cast<double>(numeric - 1);
                if (numeric >= 0 && numeric <= 11)
                    return static_cast<double>(numeric);
            }

            const auto cleaned = text.toUpperCase().removeCharacters(" ");
            for (int i = 0; i <= 11; ++i)
            {
                const auto full = modeTextForIndex(i);
                if (full.isEmpty())
                    continue;
                if (cleaned == compactDelayModeName(full).removeCharacters(" ") ||
                    cleaned == full.toUpperCase().removeCharacters(" "))
                    return static_cast<double>(i);
            }
            return modeBlock.getSlider().getValue();
        });
    repeatRateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "RepeatRate", repeatRateBlock.getSlider());

    // RATE value: the parameter's own text (the processor's free-running base
    // delay) in free mode, "SYNC" while the head divisions own the timing.
    repeatRateBlock.getSlider().setValueTextFormatter(
        [this](double value) -> juce::String
        {
            auto* tempoSyncParam = valueTreeState.getParameter("TempoSync");
            const bool syncOn = tempoSyncParam != nullptr && tempoSyncParam->getValue() > 0.5f;
            if (syncOn)
                return "SYNC";

            auto* repeatRateParam = valueTreeState.getParameter("RepeatRate");
            if (repeatRateParam == nullptr)
                return {};
            return repeatRateParam->getText(repeatRateParam->convertTo0to1(static_cast<float>(value)), 0);
        });

    intensityAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "Intensity", intensityBlock.getSlider());
    echoLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "EchoLevel", echoLevelBlock.getSlider());
    echoLevelBlock.getSlider().setValueTextFormatter(
        [](double value)
        {
            auto text = juce::String(value, 1);
            if (text.endsWith(".0"))
                text = text.dropLastCharacters(2);
            if (value > 0.0 && !text.startsWithChar('+'))
                text = "+" + text;
            return text;
        });
    pingPongAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        valueTreeState, "PingPong", pingPongButton);
    tempoSyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        valueTreeState, "TempoSync", tempoSyncButton);

    // EchoSend is "input feeds tape"; the chip shows the inverse (dub engaged).
    if (auto* echoSendParam = valueTreeState.getParameter("EchoSend"))
    {
        dubAttachment = std::make_unique<juce::ParameterAttachment>(
            *echoSendParam, [this](float inputSendEnabled)
            { dubLamp.setToggleState(inputSendEnabled < 0.5f, juce::dontSendNotification); });
        dubLamp.onClick = [this]()
        {
            if (dubAttachment != nullptr)
                dubAttachment->setValueAsCompleteGesture(dubLamp.getToggleState() ? 0.0f : 1.0f);
        };
        dubAttachment->sendInitialUpdate();
    }
    else
    {
        dubLamp.setEnabled(false);
    }
}

void DelayPanel::resized()
{
    const UIConstants::GridScaler scaled(getLocalBounds(), UIConstants::delayGridWidth, UIConstants::delayGridHeight);

    modeBlock.setBounds(scaled(172, 53, 328, 120));
    repeatRateBlock.setBounds(scaled(8, 116, 320, 98));
    intensityBlock.setBounds(scaled(344, 116, 320, 98));
    echoLevelBlock.setBounds(scaled(564, 2, 108, 82));
    // DUB chip centred under ECHO LEVEL, 6px clear of the Feedback block. The
    // component reserves the "DUB: INPUT OFF" width (inside the panel's right
    // edge) so engaging it never moves neighbours; the drawn chip aims at the
    // knob's centre.
    {
        const auto column = scaled(564, 86, 108, 24);
        const int dubW = dubLamp.getRequiredWidth();
        const int dubX = juce::jmin(column.getCentreX() - dubW / 2, getWidth() - 4 - dubW);
        dubLamp.setBounds(dubX, column.getY(), dubW, juce::jmax(24, column.getHeight()));
        dubLamp.setChipCentreX(column.getCentreX() - dubX);
    }

    // Toggles centered under the Mode knob, between the Rate and Feedback knobs.
    // 24px minimum hit target; y adjusted by -2 to preserve vertical centering.
    // PING-PONG is 100 wide to fit the full text at scale 1.0
    // (x at 234 keeps a 7px gap to SYNC - 6px inter-control minimum)
    pingPongButton.setBounds(scaled(234, 208, 100, 24));
    tempoSyncButton.setBounds(scaled(341, 208, 85, 24));

    // Three equal head columns; title labels at the 24px minimum hit target.
    const auto titleFont =
        CRTLookAndFeel::getTerminalFontStatic(std::max(UIConstants::typeMicroSize, 15.0f * scaled.getScaleY()));
    for (int head = 0; head < kNumHeads; ++head)
    {
        const auto index = static_cast<size_t>(head);
        const int columnX = head * 224;
        headTitleLabels[index].setBounds(scaled(columnX, 257, 224, 24));
        headTitleLabels[index].setFont(titleFont);
        headValueButtons[index].setBounds(scaled(columnX, 285, 224, 33));
    }
}

void DelayPanel::updateHeadDisplays(bool tempoSyncOn, int delayMode)
{
    const auto activeHeads = DelayHeadActivation::activeHeadsForDelayMode(delayMode);

    if (tempoSyncOn)
    {
        // Sync: each head shows its division and a log2 fill of the division's
        // length, from 1/64 (shortest) to 1/1 (longest), one step per doubling.
        constexpr float minQuarters = 0.0625f;  // 1/64
        constexpr float maxQuarters = 4.0f;     // 1/1
        static constexpr std::array<const char*, kNumHeads> divisionIds{"Head1NoteDivision", "Head2NoteDivision",
                                                                        "Head3NoteDivision"};
        for (int head = 0; head < kNumHeads; ++head)
        {
            const auto index = static_cast<size_t>(head);
            auto* division = dynamic_cast<juce::AudioParameterChoice*>(valueTreeState.getParameter(divisionIds[index]));
            if (division == nullptr)
                continue;

            // The processor's note-division table (indices 0-16 are the
            // straight, triplet and dotted divisions shared with Spatial Motion).
            const float quarters = SpatialEchoAudioProcessor::getMotionNoteDivisionMultiplier(division->getIndex());
            auto& valueButton = headValueButtons[index];
            valueButton.setButtonText(division->getCurrentValueAsText());
            valueButton.setProportion(logFill(quarters, minQuarters, maxQuarters), activeHeads[index],
                                      activeHeads[index]);
        }
        return;
    }

    // Free: the processor's base delay for the current Rate, scaled per head
    // by the selected head-ratio preset. Heads round like the Rate display, so
    // H1 always shows the same number as Rate.
    auto* repeatRateParam = valueTreeState.getParameter("RepeatRate");
    if (repeatRateParam == nullptr)
        return;
    const float baseDelayMs = SpatialEchoAudioProcessor::freeRunningBaseDelayMs(
        repeatRateParam->convertFrom0to1(repeatRateParam->getValue()));

    const auto* headRatioParam = valueTreeState.getRawParameterValue("HeadRatioPreset");
    const auto ratios = headRatiosForPreset(headRatioParam != nullptr ? static_cast<int>(headRatioParam->load()) : 0);
    const std::array<float, kNumHeads> headDelayMs{baseDelayMs, baseDelayMs * ratios.first,
                                                   baseDelayMs * ratios.second};

    // Log2 fill per head, from 70 ms (shortest base) to 525 ms (175 ms base
    // at the widest 1:2:3 ratio), so a RATE change is visible on every head.
    constexpr float minMs = 70.0f;
    constexpr float maxMs = 175.0f * HeadRatios::IDEALIZED_RATIO_3;
    for (int head = 0; head < kNumHeads; ++head)
    {
        const auto index = static_cast<size_t>(head);
        auto& valueButton = headValueButtons[index];
        valueButton.setButtonText(juce::String(juce::roundToInt(headDelayMs[index])) + " ms");
        // Free-mode heads are never clickable (no division menu to open).
        valueButton.setProportion(logFill(headDelayMs[index], minMs, maxMs), activeHeads[index], false);
    }
}

juce::Rectangle<int> DelayPanel::getHeadValueScreenBounds(int headNumber) const
{
    if (headNumber >= 1 && headNumber <= kNumHeads)
        return headValueButtons[static_cast<size_t>(headNumber - 1)].getScreenBounds();
    return getScreenBounds();
}

juce::Rectangle<int> DelayPanel::getWalkthroughScreenBounds(WalkthroughRegion region) const
{
    switch (region)
    {
        case WalkthroughRegion::mode:
            return modeBlock.getScreenBounds();
        case WalkthroughRegion::freeTime:
            if (const auto* tempoSync = valueTreeState.getRawParameterValue("TempoSync");
                tempoSync != nullptr && tempoSync->load() >= 0.5f)
                return tempoSyncButton.getScreenBounds();
            return repeatRateBlock.getScreenBounds();
        case WalkthroughRegion::rate:
            return repeatRateBlock.getScreenBounds();
        case WalkthroughRegion::sync:
            return tempoSyncButton.getScreenBounds();
        case WalkthroughRegion::heads:
        {
            juce::Rectangle<int> heads;
            for (int head = 0; head < kNumHeads; ++head)
            {
                const auto index = static_cast<size_t>(head);
                const auto column =
                    headTitleLabels[index].getScreenBounds().getUnion(headValueButtons[index].getScreenBounds());
                heads = heads.isEmpty() ? column : heads.getUnion(column);
            }
            return heads;
        }
        case WalkthroughRegion::feedback:
            return intensityBlock.getScreenBounds();
        case WalkthroughRegion::pingPong:
            return pingPongButton.getScreenBounds();
        case WalkthroughRegion::echoLevel:
            return echoLevelBlock.getScreenBounds();
        case WalkthroughRegion::dub:
            return dubLamp.getScreenBounds();
    }

    jassertfalse;
    return getScreenBounds();
}

void DelayPanel::updateEnabledStates(bool tempoSyncOn, int delayMode)
{
    // Grey out RepeatRate slider when tempo sync is enabled - and SAY WHY,
    // so the dead knob reads as handed-over control, not a broken control.
    auto& repeatSlider = repeatRateBlock.getSlider();
    repeatSlider.setEnabled(!tempoSyncOn);
    repeatSlider.setAlpha(tempoSyncOn ? 0.3f : 1.0f);
    repeatRateBlock.setTitleAlpha(tempoSyncOn ? 0.55f : 1.0f);
    repeatRateBlock.setLabelText(tempoSyncOn ? "Rate [sync]" : "Rate");
    repeatSlider.setTooltip(
        tempoSyncOn ? "SYNC is on: the H1/H2/H3 division menus control time. Turn SYNC off to set a free rate."
                    : kRateFreeModeTip);
    // Refresh the RATE value display after TempoSync change (updateText re-runs textFromValueFunction)
    repeatSlider.updateText();

    // Head alpha and affordance follow SYNC: in free mode the readouts are
    // inert (normal cursor, semi-dim); in sync mode they open the division
    // menu (pointing hand, full brightness). Heads Echo Mode excludes dim most.
    constexpr float kInertAlpha = 0.55f;  // Dim but still readable
    constexpr float kInactiveAlpha = 0.3f;
    const auto activeHeads = DelayHeadActivation::activeHeadsForDelayMode(delayMode);
    for (int head = 0; head < kNumHeads; ++head)
    {
        const auto index = static_cast<size_t>(head);
        const bool active = activeHeads[index];
        auto& valueButton = headValueButtons[index];
        headTitleLabels[index].setAlpha(active ? 1.0f : kInactiveAlpha);
        valueButton.setAlpha(active ? (tempoSyncOn ? 1.0f : kInertAlpha) : kInactiveAlpha);
        valueButton.setEnabled(active);
        valueButton.setMouseCursor(active && tempoSyncOn ? juce::MouseCursor::PointingHandCursor
                                                         : juce::MouseCursor::NormalCursor);
    }
}
