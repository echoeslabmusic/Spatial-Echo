// PreampTests: Preamp test function declarations (harmonics, aliasing, frequency response).

#pragma once

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include <JuceHeader.h>

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::Preamp
{
juce::String runPreampHarmonicsTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampAliasTest();
juce::String runPreampImdTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampFreqResponseTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampNullTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampUnityGainTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampHarmonicSignatureTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampAliasingSweepTest();
juce::String runPreampDCOffsetAndAutomationTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampDenormalSafetyTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampOversamplingLowpassCheckTest();
juce::String runPreampInputLevelMappingTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampLowLevelDistortionTest(SpatialEchoAudioProcessor& processor);
// Preamp input-loading probe (a measurement)
juce::String runPreampInputLoadingCornerProbe(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::Preamp

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
