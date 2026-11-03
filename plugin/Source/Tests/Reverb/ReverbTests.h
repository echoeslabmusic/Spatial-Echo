// ReverbTests (declarations): Test function signatures for reverb DSP validation (decay, spectral, switching,
//   stability).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::Reverb
{
juce::String runHeadroomTest(SpatialEchoAudioProcessor& processor);

// B1: Reverb-only mode routing test (classic)
juce::String runReverbOnlyModeTest(SpatialEchoAudioProcessor& processor);

// Determinism/null test for reverb path (reset -> identical output)
juce::String runReverbNullTest(SpatialEchoAudioProcessor& processor);

// Validate that higher decay settings produce longer tails than lower settings.
juce::String runReverbDecayTimeTest(SpatialEchoAudioProcessor& processor);

// Validate Decay control through the full EchoesModel signal path.
juce::String runReverbDecayProcessorPathTest(SpatialEchoAudioProcessor& processor);

// Sweep structural parameters during playback and assert click-safe continuity.
juce::String runReverbParamSweepStabilityTest(SpatialEchoAudioProcessor& processor);

// Active topology switches should fade the existing tail instead of dropping it.
juce::String runActiveReverbEngineSwitchContinuityTest(SpatialEchoAudioProcessor& processor);

// Cloud input filters must retain their recursive state when unchanged
// parameters are re-applied at arbitrary callback boundaries.
juce::String runCloudFilterStateContinuityTest(SpatialEchoAudioProcessor& processor);

// Disabled reverb should not preserve a stale tank that returns on re-enable.
juce::String runReverbDisableTailDrainStateTest(SpatialEchoAudioProcessor& processor);

// Matrix stress: sample-rate/block-size/mode/transport transitions.
juce::String runReverbStabilityMatrixTest(SpatialEchoAudioProcessor& processor);

// Direct kernel contract: requested block size must not exceed supplied vectors.
juce::String runReverbUndersizedBufferContractTest(SpatialEchoAudioProcessor& processor);

// Validate Cloud engine wet high-cut audibly attenuates high-frequency tail energy.
juce::String runCloudHighCutToneTest(SpatialEchoAudioProcessor& processor);

// Validate mono Cloud output is folded from both stereo engine outputs.
juce::String runCloudMonoFoldDownTest(SpatialEchoAudioProcessor& processor);

// Validate Cloud stereo width increases side energy/decorrelation.
juce::String runCloudStereoWidthMonotonicTest(SpatialEchoAudioProcessor& processor);

// Pin CloudSeed random buffer sequence semantics for fixed-array seed refreshes.
juce::String runCloudSeedRandomBufferDeterminismTest(SpatialEchoAudioProcessor& processor);

// A lower-rate prepare must release the previous high-rate delay storage.
juce::String runCloudReprepareMemoryTest(SpatialEchoAudioProcessor& processor);

// Feedback timing and decay must depend on the tank, not callback history.
juce::String runCloudFeedbackPartitionTest(SpatialEchoAudioProcessor& processor);

// Validate dry-input keyed ducking attenuates only the audible reverb branch and recovers.
juce::String runReverbDuckingWetBranchTest(SpatialEchoAudioProcessor& processor);

// Alias-focused stress test (HF excitation + aggressive automation).
juce::String runReverbAliasStressTest(SpatialEchoAudioProcessor& processor);

// Size-only alias stress test for reverb tank-size automation.
juce::String runReverbSizeAliasStressTest(SpatialEchoAudioProcessor& processor);

// Diagnostic: per-preset stability/peak/RMS + stress-preset isolation
juce::String runPresetStressDiagnostic(SpatialEchoAudioProcessor& processor);

// Fractional-delay readers must realise the delay they are asked for, keep
// unit magnitude, and stay continuous under modulation.
juce::String runReverbFractionalDelayReaderTest(SpatialEchoAudioProcessor& processor);

// The spring driver model's emphasis pair must be an exact inverse, i.e.
// unity gain and flat for small signals.
juce::String runSpringDriverSaturationFlatnessTest(SpatialEchoAudioProcessor& processor);

// The reduced-cost Spring topology must not change under audio when the
// host varies its callback size.
juce::String runSpringPerformanceModeLatchTest(SpatialEchoAudioProcessor& processor);

// A spring Size morph must complete during processing without allocating.
juce::String runSpringSizeMorphAllocationTest(SpatialEchoAudioProcessor& processor);

// Silence-in/silence-out and cross-process determinism of the reverb.
juce::String runReverbSilenceAndDeterminismTest(SpatialEchoAudioProcessor& processor);

// Time-domain precision audit of the wet recovery biquads.
juce::String runReverbWetFilterPrecisionTest(SpatialEchoAudioProcessor& processor);

// Input-to-output insertion gain contract for the routing code.
juce::String runReverbInsertionGainProbe(SpatialEchoAudioProcessor& processor);

}  // namespace SpatialEchoTests::Reverb

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
