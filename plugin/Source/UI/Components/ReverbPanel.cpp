// ReverbPanel: Reverb controls panel with knob blocks (size, decay, tone, level) and routing button.

#include "ReverbPanel.h"
#include "../UIConstants.h"
#include "../../ParameterWriteHelpers.h"
#include "CRTLookAndFeel.h"

ReverbPanel::ReverbPanel(juce::AudioProcessorValueTreeState& apvts)
    : valueTreeState(apvts)
{
    // Size's label and tooltip follow the engine (syncEngineLabels).
    sizeBlock.getSlider().setTextBoxStyle(juce::Slider::TextBoxBelow, true, 80, UIConstants::textBoxHeight);
    addAndMakeVisible(sizeBlock);

    decayBlock.setLabelText("Decay");
    decayBlock.getSlider().setTextBoxStyle(juce::Slider::TextBoxBelow, true, 80, UIConstants::textBoxHeight);
    decayBlock.getSlider().setTooltip("Reverb tail length, 0.4 s to 8.0 s. Higher values sustain the reverb longer.");
    addAndMakeVisible(decayBlock);

    toneBlock.setLabelText("TONE");
    toneBlock.getSlider().setTextBoxStyle(juce::Slider::TextBoxBelow, true, 80, UIConstants::textBoxHeight);
    toneBlock.getSlider().setTooltip("Tone (dark to bright).");
    addAndMakeVisible(toneBlock);

    reverbLevelBlock.setLabelText("Reverb Level");
    reverbLevelBlock.setTitleMetrics(10, UIConstants::typeMicroSize);
    reverbLevelBlock.getSlider().setTextBoxStyle(juce::Slider::TextBoxBelow, true, 70, UIConstants::textBoxHeight);
    reverbLevelBlock.getSlider().setNumDecimalPlacesToDisplay(1);
    constexpr float kHalfSweep = juce::MathConstants<float>::twoPi / 3.0f;  // 12 o'clock center, +/-120 degrees
    // Angles offset by 2*pi: same geometry, but keeps both radians
    // non-negative as setRotaryParameters' contract requires.
    reverbLevelBlock.getSlider().setRotaryParameters(juce::MathConstants<float>::twoPi - kHalfSweep,
                                                     juce::MathConstants<float>::twoPi + kHalfSweep, true);
    reverbLevelBlock.getSlider().setTooltip("Reverb branch level trim (-12 dB to +12 dB).");
    addAndMakeVisible(reverbLevelBlock);

    // Routing click-through button
    routingButton.setColour(juce::TextButton::textColourOffId, CRTLookAndFeel::getTerminalGreen());
    routingButton.setColour(juce::TextButton::textColourOnId, CRTLookAndFeel::getTerminalGreen());
    routingButton.setColour(juce::TextButton::buttonColourId, CRTLookAndFeel::getVoidBlack().withAlpha(0.3f));
    routingButton.setColour(juce::TextButton::buttonOnColourId, CRTLookAndFeel::getVoidBlack().withAlpha(0.3f));
    routingButton.setTooltip(juce::String(juce::CharPointer_UTF8(
        "Signal routing between echo and reverb. Parallel = both fed independently. Echo\u2192Reverb = reverb blooms "
        "on the echo repeats. Reverb\u2192Echo = delay taps cut through a reverberant wash. Click to cycle.")));
    routingButton.setTitle("Reverb Routing");
    routingButton.onClick = [this]() { cycleRouting(); };
    addAndMakeVisible(routingButton);

    // Explicit focus order (Accessibility: WCAG 2.1.1)
    sizeBlock.getSlider().setExplicitFocusOrder(1);
    decayBlock.getSlider().setExplicitFocusOrder(2);
    toneBlock.getSlider().setExplicitFocusOrder(3);
    routingButton.setExplicitFocusOrder(4);
    reverbLevelBlock.getSlider().setExplicitFocusOrder(5);

    // Component names identify controls in layout diagnostics.
    sizeBlock.setSliderName("sizeSlider");
    decayBlock.setSliderName("decaySlider");
    toneBlock.setSliderName("toneSlider");
    reverbLevelBlock.setSliderName("reverbLevelSlider");
    routingButton.setName("routingButton");

    // Create attachments LAST (after all component setup)
    sizeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbSize", sizeBlock.getSlider());
    decayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbDecay", decayBlock.getSlider());
    toneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbTone", toneBlock.getSlider());
    reverbLevelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        valueTreeState, "ReverbLevel", reverbLevelBlock.getSlider());
    reverbLevelBlock.getSlider().setValueTextFormatter(
        [](double value)
        {
            auto text = juce::String(value, 1);
            if (text.endsWith(".0"))
                text = text.dropLastCharacters(2);
            if (value > 0.0 && !text.startsWithChar('+'))
                text = "+" + text;
            return text;
        });

    // Routing uses direct parameter access (click-through, not slider)
    routingValue = valueTreeState.getRawParameterValue("ReverbRouting");
    engineValue = valueTreeState.getRawParameterValue("ReverbEngine");
    syncRoutingDisplay();
    syncEngineLabels();
    startTimerHz(15);
}

ReverbPanel::~ReverbPanel()
{
    stopTimer();
}

juce::Rectangle<int> ReverbPanel::getWalkthroughOverviewScreenBounds() const
{
    return getWalkthroughSizeDecayScreenBounds()
        .getUnion(getWalkthroughToneScreenBounds())
        .getUnion(getWalkthroughLevelScreenBounds())
        .getUnion(getWalkthroughRoutingScreenBounds());
}

juce::Rectangle<int> ReverbPanel::getWalkthroughSizeDecayScreenBounds() const
{
    return sizeBlock.getScreenBounds().getUnion(decayBlock.getScreenBounds());
}

juce::Rectangle<int> ReverbPanel::getWalkthroughToneScreenBounds() const
{
    return toneBlock.getScreenBounds();
}

juce::Rectangle<int> ReverbPanel::getWalkthroughLevelScreenBounds() const
{
    return reverbLevelBlock.getScreenBounds();
}

void ReverbPanel::resized()
{
    const UIConstants::GridScaler scaled(getLocalBounds(), UIConstants::reverbGridWidth, UIConstants::reverbGridHeight);

    // Routing button: centered under the REVERB dropdown label area
    // Height at the 24px minimum hit target; y adjusted by -1 to recenter
    constexpr int btnW = 160;
    constexpr int btnH = 24;
    constexpr int btnY = 45;
    constexpr int btnX = (UIConstants::reverbGridWidth - btnW) / 2;
    routingButton.setBounds(scaled(btnX, btnY, btnW, btnH));

    // 3 knobs evenly spaced: Size, Decay, Tone
    constexpr int knobW = 140;
    constexpr int knobH = 105;
    constexpr int knobY = 85;
    constexpr int totalKnobsW = knobW * 3;
    constexpr int gap = (UIConstants::reverbGridWidth - totalKnobsW) / 4;
    constexpr int x0 = gap;
    constexpr int x1 = x0 + knobW + gap;
    constexpr int x2 = x1 + knobW + gap;

    sizeBlock.setBounds(scaled(x0, knobY, knobW, knobH));
    decayBlock.setBounds(scaled(x1, knobY, knobW, knobH));
    toneBlock.setBounds(scaled(x2, knobY, knobW, knobH));
    reverbLevelBlock.setBounds(scaled(564, 2, 108, 82));
}

void ReverbPanel::timerCallback()
{
    syncRoutingDisplay();
    syncEngineLabels();
}

void ReverbPanel::cycleRouting()
{
    auto* param = valueTreeState.getParameter("ReverbRouting");
    if (param == nullptr)
        return;

    const int current = juce::roundToInt(param->convertFrom0to1(param->getValue()));
    const int next = (current + 1) % 3;
    SpatialEchoParameterWrite::setChoiceById(valueTreeState, "ReverbRouting", next, true);
    syncRoutingDisplay();
}

void ReverbPanel::syncEngineLabels()
{
    const int currentEngine = (engineValue != nullptr) ? static_cast<int>(engineValue->load()) : 0;
    if (currentEngine == lastEngineIndex)
        return;

    lastEngineIndex = currentEngine;
    const bool isSpring = (currentEngine == 0);

    sizeBlock.setLabelText(isSpring ? "Size" : "Room Size");
    sizeBlock.getSlider().setTooltip(
        isSpring ? "Spring tank size. Larger values give a bigger, more spacious spring response with more late tail."
                 : "Virtual room size. Larger values produce longer, more spacious reflections.");
}

void ReverbPanel::syncRoutingDisplay()
{
    if (routingValue == nullptr)
        return;

    const int index = juce::roundToInt(routingValue->load());
    // Plain ASCII: the terminal font has no glyphs for the cycle/arrow
    // codepoints and renders garbage. The tooltip
    // carries the click-to-cycle affordance.
    static const juce::StringArray names{"PARALLEL", "ECHO > REVERB", "REVERB > ECHO"};
    const auto& text = names[juce::jlimit(0, 2, index)];

    if (routingButton.getButtonText() != text)
        routingButton.setButtonText(text);
}
