/*
  ==============================================================================

    DisplayNameTests.cpp
    Host-facing parameter display-name contracts (spelled-out names).

  ==============================================================================
*/

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include <JuceHeader.h>
#include "SystemTests.h"
#include "../../PluginProcessor.h"

namespace SpatialEchoTests::System
{
juce::String runDisplayNameRolloutTest(SpatialEchoAudioProcessor& processor)
{
    auto& vts = processor.getValueTreeState();

    // Verify display names are spelled out
    auto checkName = [&](const char* paramId, const juce::String& expectedDisplay) -> juce::String
    {
        auto* param = vts.getParameter(paramId);
        if (!param)
            return "DisplayNameRollout: Parameter " + juce::String(paramId) + " not found [FAIL]";

        if (param->getName(256) != expectedDisplay)
            return "DisplayNameRollout: " + juce::String(paramId) + " has name '" + param->getName(256) +
                   "', expected '" + expectedDisplay + "' [FAIL]";

        return "";
    };

    auto result = checkName("HeadPositionMode", "Head Position Mode");
    if (result.isNotEmpty())
        return result;

    // Test Head 1 parameters
    result = checkName("Head1Azimuth", "Head 1 Azimuth");
    if (result.isNotEmpty())
        return result;

    result = checkName("Head1Elevation", "Head 1 Elevation");
    if (result.isNotEmpty())
        return result;

    result = checkName("Head1Distance", "Head 1 Distance");
    if (result.isNotEmpty())
        return result;

    // Test Head 2 parameters
    result = checkName("Head2Azimuth", "Head 2 Azimuth");
    if (result.isNotEmpty())
        return result;

    result = checkName("Head2Elevation", "Head 2 Elevation");
    if (result.isNotEmpty())
        return result;

    result = checkName("Head2Distance", "Head 2 Distance");
    if (result.isNotEmpty())
        return result;

    // Test Head 3 parameters
    result = checkName("Head3Azimuth", "Head 3 Azimuth");
    if (result.isNotEmpty())
        return result;

    result = checkName("Head3Elevation", "Head 3 Elevation");
    if (result.isNotEmpty())
        return result;

    result = checkName("Head3Distance", "Head 3 Distance");
    if (result.isNotEmpty())
        return result;

    return "DisplayNameRollout: [PASS]";
}

juce::String runUserVisibleStringUpdateTest(SpatialEchoAudioProcessor& processor)
{
    // Lightweight terminology check: the head-position parameter carries
    // the spelled-out display name (no "PER-TAP" strings survive).
    auto& vts = processor.getValueTreeState();
    auto* param = vts.getParameter("HeadPositionMode");

    if (!param)
        return "UserVisibleStringUpdate: HeadPositionMode parameter not found [FAIL]";

    const auto displayName = param->getName(256);
    if (displayName != "Head Position Mode")
        return "UserVisibleStringUpdate: HeadPositionMode display name is '" + displayName +
               "', expected 'Head Position Mode' [FAIL]";

    return "UserVisibleStringUpdate: [PASS]";
}
}  // namespace SpatialEchoTests::System

#endif
