// FeedbackTapeCalibrationTests: User-domain level/decay/tone calibration measurements.

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::TapeEmulation
{
juce::String runTapeRecordSmallSignalMatrixTest(SpatialEchoAudioProcessor& processor);
juce::String runTapeRecordMakeupCalibrationTest(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
