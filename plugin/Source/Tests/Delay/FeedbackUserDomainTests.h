// FeedbackUserDomainTests: Public-control feedback, first-repeat, and echo-bus calibration contracts.

#pragma once

#include <JuceHeader.h>

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::Delay
{
juce::String runUserFeedbackDecayProfileTest(SpatialEchoAudioProcessor& processor);
juce::String runFirstRepeatToneDarkeningTest(SpatialEchoAudioProcessor& processor);
juce::String runFirstRepeatLevelSpectrumTest(SpatialEchoAudioProcessor& processor);
juce::String runEchoPresentationHeadroomTest(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::Delay

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
