// MotionCardStrip: Horizontal strip of named motion-pattern cards with animated trajectory previews; one click applies
//   the pattern's parameter set atomically. Hosted by SpatialMotionSection.

#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>

/**
 * MotionCardStrip - radio-group selector for the MotionPattern parameter.
 *
 * One card per pattern (Still, Orbit, Pendulum, Figure-8, Breathe, Wander).
 * Clicking a card sets MotionPattern via APVTS attachment with
 * beginChangeGesture/endChangeGesture for host automation. Cards render
 * live animated micro-previews of each pattern's trajectory by calling
 * the shared evaluator (SpatialMotionTrajectory::evaluateTrajectory),
 * reading MotionDepth from the processor, so preview and DSP result agree.
 *
 * Cards deliberately do NOT own rate/sync - those are the RATE/DEPTH macros
 * in SpatialMotionSection. Selection state comes FROM the MotionPattern
 * parameter (never state-matching), so automation and preset loads stay truthful.
 */
class MotionCardStrip : public juce::Component, private juce::Timer
{
public:
    explicit MotionCardStrip(juce::AudioProcessorValueTreeState& apvts);
    ~MotionCardStrip() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    // Fired by the trailing "+ CUSTOM" card (opens the FINE-TUNE drawer).
    std::function<void()> onCustomRequested;

private:
    struct PatternDef
    {
        const char* name;
        const char* hint;
    };
    static constexpr std::array<PatternDef, 6> kPatterns = {{
        {"STILL", "no motion"},
        {"ORBIT", "continuous rotation"},
        {"PENDULUM", "left and right swing"},
        {"FIGURE-8", "elegant lissajous"},
        {"BREATHE", "closer and farther"},
        {"WANDER", "organic random drift"},
    }};

    class CardComponent : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        CardComponent(MotionCardStrip& ownerStrip, int cardIndex);
        void paint(juce::Graphics& g) override;
        void mouseEnter(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent& event) override;
        bool keyPressed(const juce::KeyPress&) override;
        void focusGained(FocusChangeType) override { repaint(); }
        void focusLost(FocusChangeType) override { repaint(); }
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    private:
        MotionCardStrip& owner;
        const int index;
        bool hovered = false;
    };

    // Sets MotionPattern to this card's index via APVTS attachment.
    void selectCard(int index);
    // Returns the currently selected card index (from MotionPattern param).
    int getSelectedCardIndex() const;

    float readFloat(const char* paramId) const;
    int readChoice(const char* paramId) const;

    void timerCallback() override;

    juce::AudioProcessorValueTreeState& valueTreeState;
    // Cards are direct children sized to always fit the strip width, so no
    // card is ever half-hidden behind a scrollbar.
    std::vector<std::unique_ptr<CardComponent>> cardComponents;
    std::unique_ptr<juce::ParameterAttachment> patternAttachment;
    int selectedIndex = -1;
    double previewPhase = 0.0;  // cycles; advanced by the 30 Hz timer

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MotionCardStrip)
};
