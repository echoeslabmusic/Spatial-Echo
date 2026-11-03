// ToneStackTests (declarations): Test function signatures for tone EQ (boost/cut, frequency response, spectral shape).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::ToneStack
{
juce::String runLowBoostTest(SpatialEchoAudioProcessor& processor);
juce::String runHighBoostTest(SpatialEchoAudioProcessor& processor);
juce::String runToneInteractionTest(SpatialEchoAudioProcessor& processor);
juce::String runToneBypassTest(SpatialEchoAudioProcessor& processor);
juce::String runToneAutomationSmoothingTest(SpatialEchoAudioProcessor& processor);
juce::String runToneBufferBoundsContractTest(SpatialEchoAudioProcessor& processor);
juce::String runBiquadLowFrequencyNumericsTest(SpatialEchoAudioProcessor& processor);
juce::String runToneStackSumLinearityTest(SpatialEchoAudioProcessor& processor);
juce::String runToneShelfReciprocityTest(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::ToneStack

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
