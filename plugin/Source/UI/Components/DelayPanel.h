// DelayPanel: UI panel containing delay mode, repeat rate, intensity, ping-pong, and tempo-sync controls.

#pragma once
#include <JuceHeader.h>
#include <array>
#include "ControlBlocks.h"
#include "CRTLookAndFeel.h"
#include "LampButton.h"
#include "ParameterValueLayout.h"

// HeadValueButton: displays head delay time with proportional phosphor fill
class HeadValueButton : public juce::TextButton
{
public:
    HeadValueButton() = default;

    void setProportion(float fill01, bool isActive, bool isClickable = false)
    {
        const float newFill = juce::jlimit(0.0f, 1.0f, fill01);
        if (juce::approximatelyEqual(newFill, fillProportion) && dimmed == !isActive && clickable == isClickable)
            return;
        fillProportion = newFill;
        dimmed = !isActive;
        clickable = isClickable;
        repaint();
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Background
        auto bgColour = CRTLookAndFeel::getVoidBlack();
        g.setColour(bgColour.withAlpha(0.6f));
        g.fillRoundedRectangle(bounds, 2.0f);

        // Proportional phosphor fill (left to right, vertical bar)
        if (fillProportion > 0.0f)
        {
            auto green = CRTLookAndFeel::getTerminalGreen();
            float alpha = dimmed ? 0.15f : 0.35f;
            // Brighten on hover/press only in clickable mode (sync mode affordance)
            if (clickable && shouldDrawButtonAsHighlighted)
                alpha *= 1.2f;
            if (clickable && shouldDrawButtonAsDown)
                alpha *= 1.4f;
            g.setColour(green.withAlpha(juce::jlimit(0.0f, 1.0f, alpha)));

            float fillWidth = bounds.getWidth() * fillProportion;
            juce::Rectangle<float> fillArea(bounds.getX(), bounds.getY(), fillWidth, bounds.getHeight());
            g.fillRoundedRectangle(fillArea, 2.0f);
        }

        // Border
        auto borderColour = CRTLookAndFeel::getTerminalGreen();
        float borderAlpha = dimmed ? 0.3f : 0.6f;
        if (clickable && shouldDrawButtonAsHighlighted)
            borderAlpha *= 1.2f;
        if (clickable && shouldDrawButtonAsDown)
            borderAlpha *= 1.4f;
        g.setColour(borderColour.withAlpha(juce::jlimit(0.0f, 1.0f, borderAlpha)));
        g.drawRoundedRectangle(bounds, 2.0f, 1.0f);

        // Text (time display) - rely on component alpha for dimming, not internal alpha
        float textAlpha = dimmed ? 0.8f : 1.0f;  // High base alpha, let component alpha do the work
        g.setColour(borderColour.withAlpha(textAlpha));
        ParameterValueLayout::drawValueText(g, *this, CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize),
                                            getButtonText(), bounds.toNearestInt(), juce::Justification::centred);
    }

    bool hasValidValueLayout() const
    {
        return ParameterValueLayout::fits(CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize),
                                          getButtonText(), getLocalBounds());
    }

    float getValueFontHeight() const
    {
        return CRTLookAndFeel::getTerminalFontStatic(UIConstants::typeValueSize).getHeight();
    }

private:
    float fillProportion = 0.0f;
    bool dimmed = false;
    bool clickable = false;
};

/**
 * DelayPanel - the delay controls of the ECHO ENGINE section.
 *
 * Contains Mode, Repeat Rate, Feedback and Echo Level knobs, the Ping-Pong
 * and Tempo Sync toggles, the DUB lamp, and per-head delay readouts that open
 * subdivision menus while SYNC is on.
 *
 * Timer-driven: the parent calls updateHeadDisplays() and
 * updateEnabledStates() from its timer to follow parameter changes.
 *
 * Safety: Components declared BEFORE attachments to ensure correct destruction order.
 */
class DelayPanel : public juce::Component
{
public:
    enum class WalkthroughRegion
    {
        mode,
        freeTime,
        rate,
        sync,
        heads,
        feedback,
        pingPong,
        echoLevel,
        dub
    };

    explicit DelayPanel(juce::AudioProcessorValueTreeState& apvts);
    ~DelayPanel() override = default;

    void resized() override;

    // Head readouts: division names (SYNC) or delay times (free), with fills.
    void updateHeadDisplays(bool tempoSyncOn, int delayMode);

    // Dims the heads Echo Mode excludes and hands RATE over while SYNC is on.
    void updateEnabledStates(bool tempoSyncOn, int delayMode);

    // Callback for head subdivision menu (parent wires this)
    std::function<void(int headNumber)> onHeadClicked;

    // Screen bounds of a head's value button (1-3) - anchors the subdivision menu.
    juce::Rectangle<int> getHeadValueScreenBounds(int headNumber) const;

    // Focused live regions used by the interactive walkthrough.
    juce::Rectangle<int> getWalkthroughScreenBounds(WalkthroughRegion region) const;

private:
    static constexpr int kNumHeads = 3;

    juce::AudioProcessorValueTreeState& valueTreeState;

    // Components declared BEFORE attachments (destroyed AFTER attachments)
    KnobBlock modeBlock;
    KnobBlock repeatRateBlock;
    KnobBlock intensityBlock;
    KnobBlock echoLevelBlock;
    // Delay-behaviour toggles, side by side under the Mode knob.
    juce::ToggleButton pingPongButton;   // PingPong param
    juce::ToggleButton tempoSyncButton;  // TempoSync param
    // DUB chip under ECHO LEVEL: amber "DUB: INPUT OFF" = new input muted (EchoSend off).
    LampButton dubLamp{"DUB", "DUB: INPUT OFF"};

    // Head readouts (H1..H3): title label over a clickable value button.
    std::array<juce::Label, kNumHeads> headTitleLabels;
    std::array<HeadValueButton, kNumHeads> headValueButtons;

    // Attachments declared LAST (destroyed FIRST, while components still exist)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> repeatRateAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> intensityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> echoLevelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> pingPongAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tempoSyncAttachment;
    std::unique_ptr<juce::ParameterAttachment> dubAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DelayPanel)
};
