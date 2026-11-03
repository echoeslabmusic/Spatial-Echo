// EditorPreferences: Per-user editor preferences and tour launch claiming.

#include "EditorPreferences.h"

namespace
{
constexpr const char* kWalkthroughSeenKey = "interactiveWalkthroughSeen";
constexpr const char* kWindowZoomKey = "windowZoom";
constexpr const char* kPerHeadHintDismissedKey = "perHeadHintDismissed";
constexpr float kMinWindowZoom = 0.75f;
}  // namespace

bool WalkthroughLaunchGate::tryClaim(bool alreadySeen) noexcept
{
    if (alreadySeen)
        return false;

    bool expected = false;
    return claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

juce::PropertiesFile::Options EditorPreferences::makeOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Spatial Echo UI";
    options.filenameSuffix = ".settings";
    options.folderName = "Echoes Lab/Spatial Echo";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.ignoreCaseOfKeyNames = false;
    options.doNotSave = false;
    options.millisecondsBeforeSaving = -1;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return options;
}

EditorPreferences::EditorPreferences()
    : properties(std::make_unique<juce::PropertiesFile>(makeOptions()))
{
}

EditorPreferences::EditorPreferences(const juce::File& settingsFile)
    : properties(std::make_unique<juce::PropertiesFile>(settingsFile, makeOptions()))
{
}

bool EditorPreferences::hasSeenWalkthrough() const
{
    return isUsable() && properties->getBoolValue(kWalkthroughSeenKey, false);
}

bool EditorPreferences::markWalkthroughSeen()
{
    if (!isUsable())
        return false;

    properties->setValue(kWalkthroughSeenKey, true);
    return properties->saveIfNeeded();
}

float EditorPreferences::getWindowZoom() const
{
    if (!isUsable())
        return 1.0f;

    return juce::jlimit(kMinWindowZoom, 1.0f, static_cast<float>(properties->getDoubleValue(kWindowZoomKey, 1.0)));
}

bool EditorPreferences::setWindowZoom(float zoom)
{
    if (!isUsable())
        return false;

    properties->setValue(kWindowZoomKey, juce::jlimit(kMinWindowZoom, 1.0f, zoom));
    return properties->saveIfNeeded();
}

bool EditorPreferences::isPerHeadHintDismissed() const
{
    return isUsable() && properties->getBoolValue(kPerHeadHintDismissedKey, false);
}

bool EditorPreferences::markPerHeadHintDismissed()
{
    if (!isUsable())
        return false;

    properties->setValue(kPerHeadHintDismissedKey, true);
    return properties->saveIfNeeded();
}

juce::File EditorPreferences::getDefaultSettingsFile()
{
    return makeOptions().getDefaultFile();
}

WalkthroughLaunchGate& EditorPreferences::getProcessLaunchGate()
{
    static WalkthroughLaunchGate gate;
    return gate;
}
