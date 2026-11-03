// DelayTests: Tape delay test function declarations (feedback, head ratios, stability).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

class SpatialEchoAudioProcessor;

namespace SpatialEchoTests::Delay
{
// Feedback/Intensity Tests
juce::String runMinIntensityTest(SpatialEchoAudioProcessor& processor);
juce::String runDecayRateTest(SpatialEchoAudioProcessor& processor);
juce::String runSelfOscillationTest(SpatialEchoAudioProcessor& processor);
juce::String runSelfOscillationAllHeadsTest(SpatialEchoAudioProcessor& processor);
juce::String runOscillationThresholdTest(SpatialEchoAudioProcessor& processor);
juce::String runFeedbackCompensationLoopTest(SpatialEchoAudioProcessor& processor);
juce::String runFeedbackCompensationOnsetCharacterTest(SpatialEchoAudioProcessor& processor);
juce::String runIntensitySmoothingTimebaseTest(SpatialEchoAudioProcessor& processor);

// Head Timing/Routing Tests
juce::String runCircularBufferDelayIndexingTest(SpatialEchoAudioProcessor& processor);
juce::String runHeadRatiosTest(SpatialEchoAudioProcessor& processor);
juce::String runWowFlutterHeadTransportScaleTest(SpatialEchoAudioProcessor& processor);
juce::String runWowFlutterTempoSyncHeadTransportScaleTest(SpatialEchoAudioProcessor& processor);
juce::String runHeadRatiosOutputTest(SpatialEchoAudioProcessor& processor);
juce::String runModeRoutingTest(SpatialEchoAudioProcessor& processor);
juce::String runPingPongStereoEnergyPreservationTest(SpatialEchoAudioProcessor& processor);
juce::String runNormalStereoChannelIsolationTest(SpatialEchoAudioProcessor& processor);
juce::String runNormalStereoPolarityPreservationTest(SpatialEchoAudioProcessor& processor);
juce::String runTempoSyncTest(SpatialEchoAudioProcessor& processor);
juce::String runTempoSyncTransportCouplingTest(SpatialEchoAudioProcessor& processor);
juce::String runMasterRegenerationHeadEnableIndependenceTest(SpatialEchoAudioProcessor& processor);
juce::String runMasterRegenerationSinglePeriodLatticeTest(SpatialEchoAudioProcessor& processor);
juce::String runHeadCrosstalkTest(SpatialEchoAudioProcessor& processor);

// Stability/Robustness Tests
juce::String runSilenceStabilityTest(SpatialEchoAudioProcessor& processor);
juce::String runNoiseFloorTest(SpatialEchoAudioProcessor& processor);
juce::String runStereoNoiseDecorrelatedTest(SpatialEchoAudioProcessor& processor);
juce::String runNoiseWowOrthogonalityTest(SpatialEchoAudioProcessor& processor);
juce::String runConditionNoiseTrendTest(SpatialEchoAudioProcessor& processor);
juce::String runConditionModulationTrendTest(SpatialEchoAudioProcessor& processor);
juce::String runWowFlutterPhaseOffsetContinuityTest(SpatialEchoAudioProcessor& processor);
juce::String runWornTapeRepeatTextureTest(SpatialEchoAudioProcessor& processor);
juce::String runTapeSurfaceNoiseSpliceDuckTest(SpatialEchoAudioProcessor& processor);
juce::String runNoiseBufferBlockCapacityTest(SpatialEchoAudioProcessor& processor);
juce::String runTapeDelayUndersizedBufferContractTest(SpatialEchoAudioProcessor& processor);
juce::String runNoRegrowthParamJumpTest(SpatialEchoAudioProcessor& processor);
juce::String runDelayStabilityMatrixTest(SpatialEchoAudioProcessor& processor);
juce::String runDelayParamSweepTest(SpatialEchoAudioProcessor& processor);
juce::String runDeterminismTest(SpatialEchoAudioProcessor& processor);
juce::String runDelayEnabledToggleClickTest(SpatialEchoAudioProcessor& processor);
juce::String runEchoSendToggleClickTest(SpatialEchoAudioProcessor& processor);
juce::String runMonoDelaySwitchRampsTest(SpatialEchoAudioProcessor& processor);
juce::String runMonoSpliceWowParityTest(SpatialEchoAudioProcessor& processor);
juce::String runKillClearFilterStateResetTest(SpatialEchoAudioProcessor& processor);

// Per-Head Tempo Sync Tests (Basic)
juce::String runDelayModeActivationTest(SpatialEchoAudioProcessor& processor);
juce::String runTempoSyncToggleTest(SpatialEchoAudioProcessor& processor);
juce::String runSubdivisionSelectionTest(SpatialEchoAudioProcessor& processor);
juce::String runSubdivisionSmoothingTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentHeadsTest(SpatialEchoAudioProcessor& processor);
juce::String runVisualFeedbackTest(SpatialEchoAudioProcessor& processor);
juce::String runModeChangeClickTest(SpatialEchoAudioProcessor& processor);

// Per-Head Tempo Sync Tests (Comprehensive)
juce::String runAllSubdivisionsTest(SpatialEchoAudioProcessor& processor);
juce::String runAllModesTempoSyncTest(SpatialEchoAudioProcessor& processor);
juce::String runExtremeCasesTest(SpatialEchoAudioProcessor& processor);
juce::String runBPMChangeTest(SpatialEchoAudioProcessor& processor);

// Sample-rate invariance gates
juce::String runTapeTimebaseSampleRateInvarianceTest(SpatialEchoAudioProcessor& processor);

// Measurement probes (own names, own gates - never hosted inside another test)
juce::String runDelayPathLevelProbe(SpatialEchoAudioProcessor& processor);
juce::String runWowFlutterNoiseTermProbe(SpatialEchoAudioProcessor& processor);

// Reverb-injection noise bed contract (per-sample, decorrelated, rate-invariant)
juce::String runReverbNoiseBedContractTest(SpatialEchoAudioProcessor& processor);
}  // namespace SpatialEchoTests::Delay

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
