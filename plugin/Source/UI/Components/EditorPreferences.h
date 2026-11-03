// EditorPreferences: Per-user editor preferences (tour seen, window zoom, per-head hint) and tour launch claiming.

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>

// Lets only one editor per host process open the first-run tour automatically.
class WalkthroughLaunchGate
{
public:
    bool tryClaim(bool alreadySeen) noexcept;

private:
    std::atomic<bool> claimed{false};
};

/**
 * EditorPreferences - user-level settings file for editor state that belongs
 * to the person, not to a session or preset: whether the interactive tour has
 * been seen, the window zoom, and whether the per-head hint was dismissed.
 * Deliberately separate from APVTS, presets, and host project state.
 */
class EditorPreferences
{
public:
    EditorPreferences();
    explicit EditorPreferences(const juce::File& settingsFile);

    bool hasSeenWalkthrough() const;
    bool markWalkthroughSeen();

    // Editor zoom (1.0 or 0.75).
    float getWindowZoom() const;
    bool setWindowZoom(float zoom);

    // The head view's "TRY PER-HEAD" hint stops once per-head mode is used.
    bool isPerHeadHintDismissed() const;
    bool markPerHeadHintDismissed();

    static juce::File getDefaultSettingsFile();
    static WalkthroughLaunchGate& getProcessLaunchGate();

private:
    static juce::PropertiesFile::Options makeOptions();
    bool isUsable() const { return properties != nullptr && properties->isValidFile(); }

    std::unique_ptr<juce::PropertiesFile> properties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorPreferences)
};
