// TapeTests (declarations): Test function signatures for tape emulation (record saturation, modulation, and tone).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::TapeEmulation
{
// Pitch Modulation Tests
juce::String runWowRateTest(SpatialEchoAudioProcessor& processor);
juce::String runFlutterRateTest(SpatialEchoAudioProcessor& processor);
juce::String runModDepthTest(SpatialEchoAudioProcessor& processor);
juce::String runRateChangePitchTest(SpatialEchoAudioProcessor& processor);
juce::String runWowFlutterRateMappingTest(SpatialEchoAudioProcessor& processor);

juce::String runTapeAliasEstimatorQualificationTest(SpatialEchoAudioProcessor& processor);

// Aliasing/IMD under stress (high intensity + record-stage drive)
juce::String runTapeAliasingImdTest();
juce::String runTapeNoFeedbackImdBudgetTest();
juce::String runRecordChainAliasTest();
// Frequency Response Tests
juce::String runSpeedHFTest(SpatialEchoAudioProcessor& processor);
juce::String runSpeedHFBandwidthTest(SpatialEchoAudioProcessor& processor);
juce::String runConditionDarkTest(SpatialEchoAudioProcessor& processor);
juce::String runRecordChainTest(SpatialEchoAudioProcessor& processor);
juce::String runPlaybackEQTest(SpatialEchoAudioProcessor& processor);

// Deterministic baseline measurements and transition contracts
juce::String runTapeChainBaseline();
juce::String runTapePresetBaselineRenders(SpatialEchoAudioProcessor& processor);
juce::String runSelfOscBloomProfile(SpatialEchoAudioProcessor& processor);
juce::String runFeedbackSubBassAccumulation(SpatialEchoAudioProcessor& processor);
juce::String runSyncCrossfadeThresholdProbe(SpatialEchoAudioProcessor& processor);
juce::String runSyncCrossfadeRetargetTest(SpatialEchoAudioProcessor& processor);
juce::String runTapeSpliceContinuityTest(SpatialEchoAudioProcessor& processor);
juce::String runTapeNoiseTransitionTest(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
