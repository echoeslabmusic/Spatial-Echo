// Purpose: focused regressions for host latency, position publication, accessibility, and state schema.
// Realtime-safety: test-only; runs outside audio callbacks.
#pragma once
#include <JuceHeader.h>
class SpatialEchoAudioProcessor;
namespace SpatialEchoTests::System
{
juce::String runOutputLatencyContractTest(SpatialEchoAudioProcessor&);
juce::String runSpatialPositionBatchTest(SpatialEchoAudioProcessor&);
juce::String runReleaseAccessibilityTest(SpatialEchoAudioProcessor&);
juce::String runStateSchemaBoundaryTest(SpatialEchoAudioProcessor&);
}  // namespace SpatialEchoTests::System
