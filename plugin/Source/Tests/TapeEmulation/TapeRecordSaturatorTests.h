// TapeRecordSaturatorTests: Behavior-suite declarations for the standalone tape-record saturator.

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::TapeEmulation
{
juce::String runTapeRecordSaturatorBehaviorSuite();
}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
