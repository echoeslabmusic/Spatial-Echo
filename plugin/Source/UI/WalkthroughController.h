// WalkthroughController: Runs the interactive tour against the live editor: opens and closes the surfaces each step
//   teaches, resolves spotlight targets, and decides when a step's action is complete.

#pragma once

#include <JuceHeader.h>
#include <map>
#include <optional>
#include "Components/InteractiveWalkthrough.h"

class SpatialEchoAudioProcessorEditor;

/**
 * WalkthroughController - the editor-side half of the first-run tour.
 *
 * InteractiveWalkthrough draws the overlay and owns the step script; this
 * controller connects it to the editor: it expands the drawer, menu or popover
 * a step points at, maps each step's target to editor coordinates, and checks
 * whether the producer has performed the step's action (a parameter moved, a
 * preset loaded, a filter typed). It reaches into the editor's components
 * directly, as the editor's friend, because every step targets a different one.
 */
class WalkthroughController
{
public:
    using Target = InteractiveWalkthrough::Target;
    using ExpandedTarget = InteractiveWalkthrough::ExpandedTarget;
    using SpotlightDetail = InteractiveWalkthrough::SpotlightDetail;
    using CompletionSignal = InteractiveWalkthrough::CompletionSignal;

    // Wires the editor's InteractiveWalkthrough callbacks to this controller.
    explicit WalkthroughController(SpatialEchoAudioProcessorEditor& owner);
    ~WalkthroughController();

    // Clears the walkthrough callbacks; safe to call more than once.
    void detach();

    // Opens the tour once per host process for a user who has not seen it,
    // after the editor is actually on screen.
    void scheduleAutomaticStartIfEligible();
    void start(bool automatic);

    // Editor timer hook: keeps the overlay above editor children and lets it
    // re-check the current step's completion.
    void timerTick();

    // Popup menus the editor opens (tape flavor, wordmark menu). While one is
    // open the overlay stays below it so its rows remain operable.
    void setTapeMenuVisible(bool visible);
    void setAppMenuVisible(bool visible);
    bool isTapeMenuVisible() const noexcept { return tapeMenuVisible; }
    bool isAppMenuVisible() const noexcept { return appMenuVisible; }
    bool isAnyMenuVisible() const noexcept { return tapeMenuVisible || appMenuVisible; }

    // Each tape-menu open gets a generation so a stale close callback from an
    // earlier menu is ignored.
    int beginTapeMenu();
    bool isCurrentTapeMenu(int generation) const noexcept { return generation == tapeMenuGeneration; }

    void prepareStep(Target target, ExpandedTarget expandedTarget, SpotlightDetail spotlightDetail);
    std::optional<juce::Rectangle<int>> resolveTarget(Target target, ExpandedTarget expandedTarget,
                                                      SpotlightDetail spotlightDetail) const;
    bool isActionComplete(CompletionSignal completionSignal) const;

private:
    void handleDismissal(InteractiveWalkthrough::Dismissal dismissal);
    void closeTourSurfaces();
    void dismissSpatialPresetMenu();
    std::optional<juce::Rectangle<int>> resolveExpandedTarget(ExpandedTarget expandedTarget,
                                                              SpotlightDetail spotlightDetail) const;
    std::optional<juce::Rectangle<int>> resolveSurfaceTarget(Target target, SpotlightDetail spotlightDetail) const;
    std::optional<juce::Rectangle<int>> resolveCoveringSurface(ExpandedTarget expandedTarget) const;
    std::map<juce::String, float> readTrackedValues() const;
    bool anyParameterChanged(std::initializer_list<const char*> parameterIds) const;

    SpatialEchoAudioProcessorEditor& editor;
    InteractiveWalkthrough& walkthrough;

    bool autoStartEvaluated = false;
    bool autoStartScheduled = false;
    bool tapeMenuVisible = false;
    bool appMenuVisible = false;
    int tapeMenuGeneration = 0;
    juce::Component::SafePointer<juce::Component> spatialPresetMenu;

    // State captured when a step begins; completion compares against it.
    juce::String baselinePresetName;
    int baselinePresetType = 0;
    juce::String baselinePresetFilter;
    std::map<juce::String, float> baselineValues;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WalkthroughController)
};
