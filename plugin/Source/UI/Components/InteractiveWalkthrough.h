// InteractiveWalkthrough: Live first-run coach marks over the plug-in editor.

#pragma once

#include <JuceHeader.h>
#include <array>
#include <functional>
#include <optional>
#include <vector>

class InteractiveWalkthrough final : public juce::Component,
                                     private juce::FocusChangeListener,
                                     private juce::KeyListener
{
public:
    enum class Target
    {
        none,
        presets,
        tape,
        delay,
        echoEngine,
        reverb,
        reverbRouting,
        spatialPosition,
        spatialBlend,
        spatialMotion,
        mixOutput,
        utilities
    };

    enum class Dismissal
    {
        notNow,
        dontShowAgain,
        completed
    };

    enum class ExpandedTarget
    {
        none,
        presetBrowser,
        tapeMenu,
        delayDivisionMenu,
        echoEngineDrawer,
        reverbDrawer,
        spatialPresetMenu,
        spatialPositionEditor,
        spatialBlendDrawer,
        motionDivisionMenu,
        motionFineTune,
        wordmarkMenu
    };

    enum class SpotlightDetail
    {
        none,
        presetCategories,
        presetList,
        presetIntent,
        presetFilter,
        presetActions,
        presetUserPresets,
        tapeFlavors,
        tapeNoise,
        tapeDrive,
        delayMode,
        delayRate,
        delaySync,
        delayHeads,
        delayFeedback,
        delayPingPong,
        delayLevel,
        delayDub,
        echoDrawer,
        echoHeadRatio,
        echoAutoPanic,
        echoStatus,
        reverbOverview,
        reverbEngine,
        reverbPreset,
        reverbSizeDecay,
        reverbTone,
        reverbLevel,
        reverbPreDelayWidth,
        reverbCharacter,
        reverbMod,
        reverbDucking,
        reverbPin,
        reverbReset,
        spatialEnable,
        spatialPreset,
        spatialMode,
        spatialHeads,
        spatialView,
        spatialDistanceView,
        spatialReadout,
        spatialWet,
        spatialCrossover,
        spatialWidth,
        spatialChain,
        spatialQuality,
        motionPatterns,
        motionRate,
        motionDivision,
        motionDepth,
        motionChoreo,
        motionSpread,
        motionRetrig
    };

    enum class CompletionSignal
    {
        none,
        presetBrowser,
        presetAudition,
        presetFilter,
        tapeFlavor,
        tapeNoise,
        tapeDrive,
        delayMode,
        delayRate,
        delaySync,
        delayHeads,
        delayFeedback,
        delayPingPong,
        delayLevel,
        delayDub,
        echoHeadRatio,
        echoAutoPanic,
        reverbEngine,
        reverbPreset,
        reverbSizeDecay,
        reverbTone,
        reverbLevel,
        reverbPreDelayWidth,
        reverbCharacter,
        reverbMod,
        reverbDucking,
        reverbRouting,
        spatialEnable,
        spatialPreset,
        spatialMode,
        spatialPosition,
        spatialDistance,
        spatialExactPosition,
        spatialWet,
        spatialCrossover,
        spatialWidth,
        spatialChain,
        spatialQuality,
        motionPattern,
        motionRate,
        motionDivision,
        motionDepth,
        motionChoreo,
        motionSpread,
        motionRetrig,
        mixOutput,
        utilities
    };

    using TargetResolver = std::function<std::optional<juce::Rectangle<int>>(Target, ExpandedTarget, SpotlightDetail)>;
    // An open drawer or menu that draws above the tour, in this component's
    // coordinates; the card keeps clear of all of it.
    using SurfaceResolver = std::function<std::optional<juce::Rectangle<int>>(ExpandedTarget)>;
    using CompletionResolver = std::function<bool(CompletionSignal)>;

    InteractiveWalkthrough();
    ~InteractiveWalkthrough() override;

    void start();
    void refreshTarget();
    void refreshActionState();

    void setTargetResolver(TargetResolver resolver) { targetResolver = std::move(resolver); }
    void setCoveringSurfaceResolver(SurfaceResolver resolver) { coveringSurfaceResolver = std::move(resolver); }
    void setCompletionResolver(CompletionResolver resolver) { completionResolver = std::move(resolver); }
    void setExternalPopupActive(bool active);
    void attachToCurrentPopupMenu();
    void attachToCurrentModalComponent();
    void detachFromModalComponent();
    void dismissAttachedModalComponent();
    bool hasActiveAttachedModalComponent() const
    {
        return attachedModalComponent != nullptr && attachedModalComponent->isShowing() &&
               attachedModalComponent->isCurrentlyModal(false);
    }
    bool isShowingExpandedTarget(ExpandedTarget target) const
    {
        return isShowing() && current().expandedTarget == target;
    }
    void setSpotlightFocusComponent(juce::Component* component) { spotlightFocusComponent = component; }

    std::function<void(Dismissal)> onDismissed;
    std::function<void(bool)> onActiveChanged;
    std::function<void(Target, ExpandedTarget, SpotlightDetail)> onStepChanged;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    bool hitTest(int x, int y) override;
    bool keyPressed(const juce::KeyPress& key) override;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    int getCurrentStepIndexForTesting() const { return currentStep; }
    int getStepCountForTesting() const { return static_cast<int>(steps().size()); }
    Target getCurrentTargetForTesting() const { return current().target; }
    ExpandedTarget getCurrentExpandedTargetForTesting() const { return current().expandedTarget; }
    SpotlightDetail getCurrentSpotlightDetailForTesting() const { return current().spotlightDetail; }
    CompletionSignal getCurrentCompletionSignalForTesting() const { return current().completionSignal; }
    juce::Rectangle<int> getCardBoundsForTesting() const { return cardBounds; }
    juce::Rectangle<int> getSpotlightBoundsForTesting() const { return spotlightBounds; }
    bool isCurrentActionCompleteForTesting() const { return actionComplete; }
    bool isFocusAllowedForTesting(const juce::Component* component) const { return isFocusAllowed(component); }
    bool handlePopupKeyForTesting(const juce::KeyPress& key)
    {
        return keyPressed(key, attachedModalComponent.getComponent());
    }
    juce::Component* getAttachedModalComponentForTesting() const { return attachedModalComponent.getComponent(); }
    juce::String getSectionTransitionLeadForTesting() const { return getSectionTransitionLead(); }
    juce::Rectangle<int> getSectionTransitionBoundsForTesting() const { return cardBounds; }
#endif

private:
    class TertiaryButtonLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted,
                                  bool down) override;
        void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    };

    struct Step
    {
        const char* title;
        const char* body;
        const char* action;
        Target target;
        ExpandedTarget expandedTarget;
        SpotlightDetail spotlightDetail;
        CompletionSignal completionSignal;
        const char* section;
        const char* sectionTagline;
        int sectionNumber;
    };

    static const std::array<Step, 58>& steps();

    void advance();
    void goBack();
    void dismiss(Dismissal dismissal);
    void updateButtons();
    void updateAccessibilityText();
    void moveCoachFocus(bool forwards);
    void layoutCard();
    juce::Rectangle<int> chooseCardBounds() const;
    const Step& current() const;
    bool isFocusAllowed(const juce::Component* component) const;
    bool isWithinActiveExternalPopup(const juce::Component* component) const;
    void attachToModalComponent(juce::Component* component, bool includeDescendants);
    void globalFocusChanged(juce::Component* focusedComponent) override;
    bool keyPressed(const juce::KeyPress& key, juce::Component* originatingComponent) override;
    bool isWelcome() const { return currentStep == 0; }
    bool isFinish() const { return currentStep == static_cast<int>(steps().size()) - 1; }
    juce::String getSectionTransitionLead() const;
    void paintLessonContent(juce::Graphics& g, juce::Rectangle<int> content) const;
    void paintSectionTransition(juce::Graphics& g, juce::Rectangle<int> content) const;

    TargetResolver targetResolver;
    SurfaceResolver coveringSurfaceResolver;
    CompletionResolver completionResolver;

    TertiaryButtonLookAndFeel tertiaryButtonLookAndFeel;
    juce::TextButton backButton{"BACK"};
    juce::TextButton nextButton{"NEXT"};
    juce::TextButton exitButton{"EXIT"};
    juce::TextButton suppressButton{"DON'T SHOW AGAIN"};

    int currentStep = 0;
    bool actionComplete = false;
    bool externalPopupActive = false;
    bool popupCoachNavigationActive = false;
    bool focusListenerRegistered = false;
    bool restoringFocus = false;
    juce::Component::SafePointer<juce::Component> previousFocus;
    juce::Component::SafePointer<juce::Component> spotlightFocusComponent;
    juce::Component::SafePointer<juce::Component> attachedModalComponent;
    juce::Component::SafePointer<juce::Component> modalFocusBeforeCoach;
    std::vector<juce::Component::SafePointer<juce::Component>> attachedModalKeyComponents;
    float completionPulse = 0.0f;
    float attentionPhase = 0.0f;
    float sectionTransition = 0.0f;
    juce::Rectangle<int> interactionBounds;
    juce::Rectangle<int> spotlightBounds;
    juce::Rectangle<int> coveringSurfaceBounds;
    juce::Rectangle<int> cardBounds;

    static constexpr int kCardWidth = 420;
    static constexpr int kCardHeight = 270;
    static constexpr int kOuterMargin = 16;
    static constexpr int kTargetPadding = 8;
    static constexpr int kSectionCount = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InteractiveWalkthrough)
};
