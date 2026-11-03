// SystemTests (declarations): Test function signatures for processor integration, routing, serialization, and realtime
//   safety.

#pragma once

#include <JuceHeader.h>
#include "../Common/TestUtilities.h"
#include <cmath>
#include <functional>

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

class SpatialEchoAudioProcessor;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

namespace SpatialEchoTests::System
{

// Editor, paths and presets
juce::String runEqPathPlacementTest(SpatialEchoAudioProcessor& processor);
juce::String runEditorCreationSmokeTest(SpatialEchoAudioProcessor& processor);
juce::String runEditorWindowZoomTest(SpatialEchoAudioProcessor& processor);
juce::String runEditorKnobResetDefaultsTest(SpatialEchoAudioProcessor& processor);
juce::String runEditorSnapshot();
juce::String runOutputMixTextParsingTest(SpatialEchoAudioProcessor& processor);
juce::String runFreeHeadTimeReadoutTest(SpatialEchoAudioProcessor& processor);
juce::String runComboBoxPopupsFitTest(SpatialEchoAudioProcessor& processor);
juce::String runDrawerLookMatchesStandaloneTest(SpatialEchoAudioProcessor& processor);
juce::String runPresetBrowserKeyboardRoutingTest(SpatialEchoAudioProcessor& processor);
juce::String runInteractiveWalkthroughTest(SpatialEchoAudioProcessor& processor);
juce::String runWalkthroughCardClearsSurfacesTest(SpatialEchoAudioProcessor& processor);
juce::String runParameterValueNoTruncationTest(SpatialEchoAudioProcessor& processor);
// Paint-time benchmark (a measurement), in SpatialHeadViewPaintBenchTest.cpp.
juce::String runSpatialHeadViewPaintBenchTest(SpatialEchoAudioProcessor& processor);
juce::String runReverbPresetModeChoiceWriteTest(SpatialEchoAudioProcessor& processor);
juce::String runReverbPresetModeChoiceWriteTailRetentionTest(SpatialEchoAudioProcessor& processor);
juce::String runDropdownWindowPlacementBoundsTest(SpatialEchoAudioProcessor& processor);
juce::String runDropdownWindowHostOwnsChildrenTest(SpatialEchoAudioProcessor& processor);
juce::String runDenormalHandlingTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralAutomationStressTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralDiffuseEqDefaultTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralDiffuseEqQualityTailTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralQualitySwitchCapacityTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralQualitySwitchDuringCooldownTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralFrontBackSpectralCueTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralFrontSectorILDTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralEarlyReflectionsTapIsolationTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralStereoSideWetMixTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralSlerpSeamPathTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralAzimuthSmoothingBlockInvarianceTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralEarlyReflectionsShortBlocksTest(SpatialEchoAudioProcessor& processor);
juce::String runInvalidInputRecoveryTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralInterpModeRefreshTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralContinuitySweepTest(SpatialEchoAudioProcessor& processor);
juce::String runHrtfResourceConsistencyTest(SpatialEchoAudioProcessor& processor);
juce::String runBrandSealEmbeddedTest(SpatialEchoAudioProcessor& processor);
juce::String runSofaMetadataDumpTest(SpatialEchoAudioProcessor& processor);
juce::String runSofaConventionDiagnosticsTest(SpatialEchoAudioProcessor& processor);
juce::String runSofaConventionFixtureContractsTest(SpatialEchoAudioProcessor& processor);
juce::String runSofaReloadSafetyTest(SpatialEchoAudioProcessor& processor);
juce::String runMonoChannelSafetyTest(SpatialEchoAudioProcessor& processor);
juce::String runOutputProtectionTest(SpatialEchoAudioProcessor& processor);
juce::String runOutputProtectionToggleContinuityTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentLevelParityProbe(SpatialEchoAudioProcessor& processor);
juce::String runSourceWidthEnergyConservationTest(SpatialEchoAudioProcessor& processor);
juce::String runAutoPanicTest(SpatialEchoAudioProcessor& processor);
juce::String runOutputGainAutomationSmoothingTest(SpatialEchoAudioProcessor& processor);
juce::String runReverbNoiseBlockRateIndependenceTest(SpatialEchoAudioProcessor& processor);
juce::String runNoiseRoutingTimelineTest(SpatialEchoAudioProcessor& processor);
juce::String runReverbRoutingImpulseContractsTest(SpatialEchoAudioProcessor& processor);
juce::String runReverbRoutingHeadroomMatrixTest(SpatialEchoAudioProcessor& processor);
juce::String runSerialRoutingHeadroomProbe(SpatialEchoAudioProcessor& processor);
juce::String runReverbRouteReturnParity(SpatialEchoAudioProcessor& processor);
juce::String runPresetRosterRenders(SpatialEchoAudioProcessor& processor);
// Reverb probes (measurements)
juce::String runReverbMacroSmoothingFlutterProbe(SpatialEchoAudioProcessor& processor);
juce::String runReverbEngineLevelParityProbe(SpatialEchoAudioProcessor& processor);
juce::String runActiveReverbRoutingSwitchContinuityTest(SpatialEchoAudioProcessor& processor);
juce::String runBinauralNearFieldHeadroomTest(SpatialEchoAudioProcessor& processor);
juce::String runProcessorLifecycleTeardownTest(SpatialEchoAudioProcessor& processor);
juce::String runFactoryPresetCategoryIntegrityTest(SpatialEchoAudioProcessor& processor);
juce::String runFactoryPresetDescriptionCoverageTest(SpatialEchoAudioProcessor& processor);
juce::String runFactoryPresetLoadTest(SpatialEchoAudioProcessor& processor);
juce::String runDefaultPresetAutoLoadTest(SpatialEchoAudioProcessor& processor);
juce::String runPresetLoadUndoRecoveryTest(SpatialEchoAudioProcessor& processor);
juce::String runFactoryPresetNoisePolicyTest(SpatialEchoAudioProcessor& processor);
juce::String runPresetLoadSilenceRate(SpatialEchoAudioProcessor& processor);
juce::String runTapePresetNoiseEnablePolicyTest(SpatialEchoAudioProcessor& processor);
juce::String runUserPresetPersistenceTest(SpatialEchoAudioProcessor& processor);
juce::String runUserPresetSofaPathPolicyTest(SpatialEchoAudioProcessor& processor);
juce::String runMixLockStateRecallAuthorityTest(SpatialEchoAudioProcessor& processor);
juce::String runSelectorSideEffectStoppedTransportRoundTripTest(SpatialEchoAudioProcessor& processor);
juce::String runWholeStateReplacementSuspensionTest(SpatialEchoAudioProcessor& processor);
juce::String runHostTailLengthContractTest(SpatialEchoAudioProcessor& processor);
juce::String runActiveFactoryPresetSwitchContinuityTest(SpatialEchoAudioProcessor& processor);
juce::String runStartupDefaultStateConsistencyTest(SpatialEchoAudioProcessor& processor);
juce::String runStartupDefaultNoiseFloorTest(SpatialEchoAudioProcessor& processor);
juce::String runInitDefaultParameterContractTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialPresetIntegrityTest(SpatialEchoAudioProcessor& processor);
juce::String runPreampPathCoverageTest(SpatialEchoAudioProcessor& processor);
juce::String runToneStackIsolationTest(SpatialEchoAudioProcessor& processor);
juce::String runMixSafetyTest(SpatialEchoAudioProcessor& processor);
juce::String runActivePathMixMatrixTest(SpatialEchoAudioProcessor& processor);
juce::String runBranchTiltModeSwitchContinuityTest(SpatialEchoAudioProcessor& processor);
juce::String runDualPathBalanceDefaultTest(SpatialEchoAudioProcessor& processor);
juce::String runBlockSizeExtremesTest(SpatialEchoAudioProcessor& processor);
juce::String runCpuStressProfileTest(SpatialEchoAudioProcessor& processor);

// Spatial routing tests
juce::String runSpatialRoutingDifferenceTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingBypassEquivalenceTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingLatencyContractTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingBinauralImageTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialMetricBatteryTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingNoiseImageTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingModeSwitchStabilityTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingPresetRoundTripTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialRoutingMonoSafetyTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialControlSourceArbitrationTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialMotionSourceTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialMotionContinuityStressTest(SpatialEchoAudioProcessor& processor);
juce::String runSpatialMotionFeatureCoverageTest(SpatialEchoAudioProcessor& processor);

// Display-name contract tests (DisplayNameTests.cpp)
juce::String runDisplayNameRolloutTest(SpatialEchoAudioProcessor& processor);
juce::String runUserVisibleStringUpdateTest(SpatialEchoAudioProcessor& processor);

// Per-tap spatial positioning tests
juce::String runPerTapSpatialization(SpatialEchoAudioProcessor& processor);

// Per-tap metering and motion tests
juce::String runTapEnvelopeMeterTest(SpatialEchoAudioProcessor& processor);
juce::String runPerTapMonoSafety(SpatialEchoAudioProcessor& processor);
juce::String runPerTapMotionConstellation(SpatialEchoAudioProcessor& processor);
juce::String runPerTapMotionSmoothness(SpatialEchoAudioProcessor& processor);
juce::String runIndependentStereoCaptureMatrixTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentPostTreatmentParityTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentEnvelopeAuthorityTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentToneAuthorityTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentSpatialExactOnceTest(SpatialEchoAudioProcessor& processor);
juce::String runAllVoiceSpatialConfigPropagationTest(SpatialEchoAudioProcessor& processor);
juce::String runIndependentVoiceHistoryReentryTest(SpatialEchoAudioProcessor& processor);

// Trajectory shape tests (pure evaluator tests, no processor needed)
juce::String runOrbitAzimuthContinuityTest();
juce::String runFigure8FrequencyRatioTest();
juce::String runBreatheDistanceOnlyTest();
juce::String runDepthZeroStillnessTest();
juce::String runChoreographyOffsetsTest();
}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
