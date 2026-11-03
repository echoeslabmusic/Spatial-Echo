// ReverbPresetIRTest: IR measurement test function for reverb preset catalog characterization.

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::Reverb
{
// Measure all 12 preset IRs and report T60/brightness/early-late/onset
juce::String runPresetIRMeasurementTest(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::Reverb

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
