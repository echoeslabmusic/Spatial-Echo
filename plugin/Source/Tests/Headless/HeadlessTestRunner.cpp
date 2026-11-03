// HeadlessTestRunner: The registry of every headless test. Each name appears here exactly once; the runner, the suite
//   script and CTest all read the list from the runner (--list, --list-all).

#include "HeadlessTestRunner.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../PluginProcessor.h"
#include "../Binaural/BinauralSelfTests.h"
#include "../Delay/DelayTests.h"
#include "../Delay/FeedbackUserDomainTests.h"
#include "../Preamp/PreampTests.h"
#include "../Reverb/ReverbPresetIRTest.h"
#include "../Reverb/ReverbTests.h"
#include "../System/RegressionTests.h"
#include "../System/SystemTests.h"
#include "../TapeEmulation/FeedbackTapeCalibrationTests.h"
#include "../TapeEmulation/TapeRecordSaturatorTests.h"
#include "../TapeEmulation/TapeTests.h"
#include "../ToneStack/ToneStackTests.h"

#include <exception>

namespace SpatialEchoTests::Headless
{
namespace
{
TestEntry entry(const char* name, TestKind kind, ProcessorTest test)
{
    return {name, kind, test, nullptr};
}

TestEntry entry(const char* name, TestKind kind, StandaloneTest test)
{
    return {name, kind, nullptr, test};
}
}  // namespace

const std::vector<TestEntry>& registry()
{
    static const std::vector<TestEntry> tests{
        // Reverb
        entry("ReverbHeadroom", TestKind::test, Reverb::runHeadroomTest),
        entry("ReverbOnlyMode", TestKind::test, Reverb::runReverbOnlyModeTest),
        entry("ReverbDecayTime", TestKind::test, Reverb::runReverbDecayTimeTest),
        entry("ReverbDecayProcessorPath", TestKind::test, Reverb::runReverbDecayProcessorPathTest),
        entry("ReverbParamSweep", TestKind::test, Reverb::runReverbParamSweepStabilityTest),
        entry("CloudFilterStateContinuity", TestKind::test, Reverb::runCloudFilterStateContinuityTest),
        entry("ActiveReverbEngineSwitchContinuity", TestKind::test, Reverb::runActiveReverbEngineSwitchContinuityTest),
        entry("ReverbDisableTailDrainState", TestKind::test, Reverb::runReverbDisableTailDrainStateTest),
        entry("ReverbStabilityMatrix", TestKind::test, Reverb::runReverbStabilityMatrixTest),
        entry("ReverbUndersizedBufferContract", TestKind::test, Reverb::runReverbUndersizedBufferContractTest),
        entry("CloudHighCutTone", TestKind::test, Reverb::runCloudHighCutToneTest),
        entry("CloudMonoFoldDown", TestKind::test, Reverb::runCloudMonoFoldDownTest),
        entry("CloudStereoWidth", TestKind::test, Reverb::runCloudStereoWidthMonotonicTest),
        entry("CloudSeedRandomBufferDeterminism", TestKind::test, Reverb::runCloudSeedRandomBufferDeterminismTest),
        entry("ReverbDuckingWetBranch", TestKind::test, Reverb::runReverbDuckingWetBranchTest),
        entry("ReverbAliasStress", TestKind::test, Reverb::runReverbAliasStressTest),
        entry("ReverbSizeAliasStress", TestKind::test, Reverb::runReverbSizeAliasStressTest),
        entry("ReverbPresetIR", TestKind::test, Reverb::runPresetIRMeasurementTest),
        entry("CloudReprepareMemory", TestKind::test, Reverb::runCloudReprepareMemoryTest),
        entry("CloudFeedbackPartition", TestKind::test, Reverb::runCloudFeedbackPartitionTest),
        entry("ReverbFractionalDelayReader", TestKind::test, Reverb::runReverbFractionalDelayReaderTest),
        entry("SpringDriverSaturationFlatness", TestKind::test, Reverb::runSpringDriverSaturationFlatnessTest),
        entry("SpringPerformanceModeLatch", TestKind::test, Reverb::runSpringPerformanceModeLatchTest),
        entry("ReverbInsertionGain", TestKind::measurement, Reverb::runReverbInsertionGainProbe),
        entry("ReverbWetFilterPrecision", TestKind::test, Reverb::runReverbWetFilterPrecisionTest),
        entry("ReverbSilenceAndDeterminism", TestKind::test, Reverb::runReverbSilenceAndDeterminismTest),
        entry("SpringSizeMorphAllocation", TestKind::test, Reverb::runSpringSizeMorphAllocationTest),
        entry("ReverbNull", TestKind::test, Reverb::runReverbNullTest),
        entry("PresetStressDiag", TestKind::measurement, Reverb::runPresetStressDiagnostic),
        // Tape delay: feedback, heads, timing, noise
        entry("MinIntensity", TestKind::test, Delay::runMinIntensityTest),
        entry("DecayRate", TestKind::test, Delay::runDecayRateTest),
        entry("SelfOscillation", TestKind::test, Delay::runSelfOscillationTest),
        entry("SelfOscillationAllHeads", TestKind::test, Delay::runSelfOscillationAllHeadsTest),
        entry("OscillationThreshold", TestKind::test, Delay::runOscillationThresholdTest),
        entry("FeedbackCompensationLoop", TestKind::test, Delay::runFeedbackCompensationLoopTest),
        entry("FeedbackCompensationOnsetCharacter", TestKind::test, Delay::runFeedbackCompensationOnsetCharacterTest),
        entry("CircularBufferDelayIndexing", TestKind::test, Delay::runCircularBufferDelayIndexingTest),
        entry("HeadRatios", TestKind::test, Delay::runHeadRatiosTest),
        entry("WowFlutterHeadTransportScale", TestKind::test, Delay::runWowFlutterHeadTransportScaleTest),
        entry("WowFlutterTempoSyncHeadTransportScale", TestKind::test,
              Delay::runWowFlutterTempoSyncHeadTransportScaleTest),
        entry("HeadRatiosOutput", TestKind::test, Delay::runHeadRatiosOutputTest),
        entry("ModeRouting", TestKind::test, Delay::runModeRoutingTest),
        entry("PingPongStereoEnergyPreservation", TestKind::test, Delay::runPingPongStereoEnergyPreservationTest),
        entry("NormalStereoChannelIsolation", TestKind::test, Delay::runNormalStereoChannelIsolationTest),
        entry("NormalStereoPolarityPreservation", TestKind::test, Delay::runNormalStereoPolarityPreservationTest),
        entry("TempoSync", TestKind::test, Delay::runTempoSyncTest),
        entry("TempoSyncTransportCoupling", TestKind::test, Delay::runTempoSyncTransportCouplingTest),
        entry("MasterRegenerationHeadEnableIndependence", TestKind::test,
              Delay::runMasterRegenerationHeadEnableIndependenceTest),
        entry("MasterRegenerationSinglePeriodLattice", TestKind::test,
              Delay::runMasterRegenerationSinglePeriodLatticeTest),
        entry("HeadCrosstalk", TestKind::test, Delay::runHeadCrosstalkTest),
        entry("SilenceStability", TestKind::test, Delay::runSilenceStabilityTest),
        entry("NoiseFloor", TestKind::test, Delay::runNoiseFloorTest),
        entry("StereoNoiseDecorrelated", TestKind::test, Delay::runStereoNoiseDecorrelatedTest),
        entry("NoiseWowOrthogonality", TestKind::test, Delay::runNoiseWowOrthogonalityTest),
        entry("ConditionNoiseTrend", TestKind::test, Delay::runConditionNoiseTrendTest),
        entry("ConditionModulationTrend", TestKind::test, Delay::runConditionModulationTrendTest),
        entry("WowFlutterPhaseOffsetContinuity", TestKind::test, Delay::runWowFlutterPhaseOffsetContinuityTest),
        entry("WornTapeRepeatTexture", TestKind::test, Delay::runWornTapeRepeatTextureTest),
        entry("TapeSurfaceNoiseSpliceDuck", TestKind::test, Delay::runTapeSurfaceNoiseSpliceDuckTest),
        entry("NoiseBufferBlockCapacity", TestKind::test, Delay::runNoiseBufferBlockCapacityTest),
        entry("TapeDelayUndersizedBufferContract", TestKind::test, Delay::runTapeDelayUndersizedBufferContractTest),
        entry("NoRegrowthParamJump", TestKind::test, Delay::runNoRegrowthParamJumpTest),
        entry("DelayStabilityMatrix", TestKind::test, Delay::runDelayStabilityMatrixTest),
        entry("Determinism", TestKind::test, Delay::runDeterminismTest),
        entry("DelayEnabledToggleClick", TestKind::test, Delay::runDelayEnabledToggleClickTest),
        entry("EchoSendToggleClick", TestKind::test, Delay::runEchoSendToggleClickTest),
        entry("MonoDelaySwitchRamps", TestKind::test, Delay::runMonoDelaySwitchRampsTest),
        entry("MonoSpliceWowParity", TestKind::test, Delay::runMonoSpliceWowParityTest),
        entry("KillClearFilterStateReset", TestKind::test, Delay::runKillClearFilterStateResetTest),
        entry("DelayParamSweep", TestKind::test, Delay::runDelayParamSweepTest),
        entry("DelayModeActivation", TestKind::test, Delay::runDelayModeActivationTest),
        entry("TempoSyncToggle", TestKind::test, Delay::runTempoSyncToggleTest),
        entry("SubdivisionSelection", TestKind::test, Delay::runSubdivisionSelectionTest),
        entry("SubdivisionSmoothing", TestKind::test, Delay::runSubdivisionSmoothingTest),
        entry("ModeChangeClick", TestKind::test, Delay::runModeChangeClickTest),
        entry("IndependentHeads", TestKind::test, Delay::runIndependentHeadsTest),
        entry("VisualFeedback", TestKind::test, Delay::runVisualFeedbackTest),
        entry("AllSubdivisions", TestKind::test, Delay::runAllSubdivisionsTest),
        entry("AllModesTempoSync", TestKind::test, Delay::runAllModesTempoSyncTest),
        entry("ExtremeCases", TestKind::test, Delay::runExtremeCasesTest),
        entry("BPMChange", TestKind::test, Delay::runBPMChangeTest),
        entry("UserFeedbackDecayProfile", TestKind::test, Delay::runUserFeedbackDecayProfileTest),
        entry("FirstRepeatToneDarkening", TestKind::test, Delay::runFirstRepeatToneDarkeningTest),
        entry("FirstRepeatLevelSpectrum", TestKind::test, Delay::runFirstRepeatLevelSpectrumTest),
        entry("EchoPresentationHeadroom", TestKind::test, Delay::runEchoPresentationHeadroomTest),
        entry("IntensitySmoothingTimebase", TestKind::test, Delay::runIntensitySmoothingTimebaseTest),
        entry("TapeTimebaseSampleRateInvariance", TestKind::test, Delay::runTapeTimebaseSampleRateInvarianceTest),
        entry("DelayPathLevelProbe", TestKind::measurement, Delay::runDelayPathLevelProbe),
        entry("WowFlutterNoiseTermProbe", TestKind::measurement, Delay::runWowFlutterNoiseTermProbe),
        entry("ReverbNoiseBedContract", TestKind::test, Delay::runReverbNoiseBedContractTest),
        // Tape emulation: modulation, frequency response, record chain
        entry("TapeAliasEstimatorQualification", TestKind::test, TapeEmulation::runTapeAliasEstimatorQualificationTest),
        entry("WowRate", TestKind::test, TapeEmulation::runWowRateTest),
        entry("FlutterRate", TestKind::test, TapeEmulation::runFlutterRateTest),
        entry("ModDepth", TestKind::test, TapeEmulation::runModDepthTest),
        entry("RateChangePitch", TestKind::test, TapeEmulation::runRateChangePitchTest),
        entry("WowFlutterRateMapping", TestKind::test, TapeEmulation::runWowFlutterRateMappingTest),
        entry("SpeedHF", TestKind::test, TapeEmulation::runSpeedHFTest),
        entry("ConditionDark", TestKind::test, TapeEmulation::runConditionDarkTest),
        entry("RecordChain", TestKind::test, TapeEmulation::runRecordChainTest),
        entry("PlaybackEQ", TestKind::test, TapeEmulation::runPlaybackEQTest),
        entry("TapeAliasingIMD", TestKind::test, TapeEmulation::runTapeAliasingImdTest),
        entry("TapeNoFeedbackIMD", TestKind::test, TapeEmulation::runTapeNoFeedbackImdBudgetTest),
        entry("RecordChainAlias", TestKind::test, TapeEmulation::runRecordChainAliasTest),
        entry("TapeRecordSaturatorBehavior", TestKind::test, TapeEmulation::runTapeRecordSaturatorBehaviorSuite),
        entry("TapeChainBaseline", TestKind::measurement, TapeEmulation::runTapeChainBaseline),
        entry("TapePresetBaselineRenders", TestKind::measurement, TapeEmulation::runTapePresetBaselineRenders),
        entry("TapeSpliceContinuity", TestKind::test, TapeEmulation::runTapeSpliceContinuityTest),
        entry("TapeNoiseTransition", TestKind::test, TapeEmulation::runTapeNoiseTransitionTest),
        entry("SyncCrossfadeRetarget", TestKind::test, TapeEmulation::runSyncCrossfadeRetargetTest),
        entry("SelfOscBloomProfile", TestKind::measurement, TapeEmulation::runSelfOscBloomProfile),
        entry("FeedbackSubBassAccumulation", TestKind::measurement, TapeEmulation::runFeedbackSubBassAccumulation),
        entry("SyncCrossfadeThresholdProbe", TestKind::measurement, TapeEmulation::runSyncCrossfadeThresholdProbe),
        entry("SpeedHFBandwidth", TestKind::test, TapeEmulation::runSpeedHFBandwidthTest),
        entry("TapeRecordSmallSignalMatrix", TestKind::measurement, TapeEmulation::runTapeRecordSmallSignalMatrixTest),
        entry("TapeRecordMakeupCalibration", TestKind::test, TapeEmulation::runTapeRecordMakeupCalibrationTest),
        // Tone stack
        entry("LowBoost", TestKind::test, ToneStack::runLowBoostTest),
        entry("HighBoost", TestKind::test, ToneStack::runHighBoostTest),
        entry("ToneInteraction", TestKind::test, ToneStack::runToneInteractionTest),
        entry("ToneBypass", TestKind::test, ToneStack::runToneBypassTest),
        entry("ToneAutomationSmoothing", TestKind::test, ToneStack::runToneAutomationSmoothingTest),
        entry("ToneBufferBoundsContract", TestKind::test, ToneStack::runToneBufferBoundsContractTest),
        entry("BiquadLowFrequencyNumerics", TestKind::test, ToneStack::runBiquadLowFrequencyNumericsTest),
        entry("ToneStackSumLinearity", TestKind::test, ToneStack::runToneStackSumLinearityTest),
        entry("ToneShelfReciprocity", TestKind::test, ToneStack::runToneShelfReciprocityTest),
        // Preamp
        entry("PreampHarmonics", TestKind::test, Preamp::runPreampHarmonicsTest),
        entry("PreampAlias", TestKind::test, Preamp::runPreampAliasTest),
        entry("PreampImd", TestKind::test, Preamp::runPreampImdTest),
        entry("PreampFreqResponse", TestKind::test, Preamp::runPreampFreqResponseTest),
        entry("PreampNull", TestKind::test, Preamp::runPreampNullTest),
        entry("PreampUnityGain", TestKind::test, Preamp::runPreampUnityGainTest),
        entry("PreampHarmonicSignature", TestKind::test, Preamp::runPreampHarmonicSignatureTest),
        entry("PreampAliasingSweep", TestKind::test, Preamp::runPreampAliasingSweepTest),
        entry("PreampDCOffsetAndAutomation", TestKind::test, Preamp::runPreampDCOffsetAndAutomationTest),
        entry("PreampDenormalSafety", TestKind::test, Preamp::runPreampDenormalSafetyTest),
        entry("PreampOversamplingLowpassCheck", TestKind::test, Preamp::runPreampOversamplingLowpassCheckTest),
        entry("PreampInputLevelMapping", TestKind::test, Preamp::runPreampInputLevelMappingTest),
        entry("PreampLowLevelDistortion", TestKind::test, Preamp::runPreampLowLevelDistortionTest),
        entry("PreampInputLoadingCornerProbe", TestKind::measurement, Preamp::runPreampInputLoadingCornerProbe),
        // Binaural renderer and spatial placement
        entry("BinauralControlContinuity", TestKind::test, BinauralProcessorTestAccess::runControlContinuitySelfTest),
        entry("BinauralAutomation", TestKind::test, System::runBinauralAutomationStressTest),
        entry("BinauralDiffuseEqDefault", TestKind::test, System::runBinauralDiffuseEqDefaultTest),
        entry("BinauralDiffuseEqQualityTail", TestKind::test, System::runBinauralDiffuseEqQualityTailTest),
        entry("BinauralQualitySwitchCapacity", TestKind::test, System::runBinauralQualitySwitchCapacityTest),
        entry("BinauralQualitySwitchDuringCooldown", TestKind::test,
              System::runBinauralQualitySwitchDuringCooldownTest),
        entry("BinauralFrontBackSpectralCue", TestKind::test, System::runBinauralFrontBackSpectralCueTest),
        entry("BinauralFrontSectorILD", TestKind::test, System::runBinauralFrontSectorILDTest),
        entry("BinauralEarlyReflectionsTapIsolation", TestKind::test,
              System::runBinauralEarlyReflectionsTapIsolationTest),
        entry("BinauralStereoSideWetMix", TestKind::test, System::runBinauralStereoSideWetMixTest),
        entry("BinauralSlerpSeamPath", TestKind::test, System::runBinauralSlerpSeamPathTest),
        entry("BinauralAzimuthSmoothingBlockInvariance", TestKind::test,
              System::runBinauralAzimuthSmoothingBlockInvarianceTest),
        entry("BinauralEarlyReflectionsShortBlocks", TestKind::test,
              System::runBinauralEarlyReflectionsShortBlocksTest),
        entry("BinauralInterpModeRefresh", TestKind::test, System::runBinauralInterpModeRefreshTest),
        entry("BinauralContinuity", TestKind::test, System::runBinauralContinuitySweepTest),
        entry("HrtfResourceConsistency", TestKind::test, System::runHrtfResourceConsistencyTest),
        entry("SofaMetadataDump", TestKind::measurement, System::runSofaMetadataDumpTest),
        entry("SofaConventionDiagnostics", TestKind::test, System::runSofaConventionDiagnosticsTest),
        entry("SofaConventionFixtureContracts", TestKind::test, System::runSofaConventionFixtureContractsTest),
        entry("SofaReloadSafety", TestKind::test, System::runSofaReloadSafetyTest),
        entry("MonoSafety", TestKind::test, System::runMonoChannelSafetyTest),
        entry("BinauralNearFieldHeadroom", TestKind::test, System::runBinauralNearFieldHeadroomTest),
        entry("BinauralConvolutionParity", TestKind::test, BinauralProcessorTestAccess::runConvolutionParitySelfTest),
        entry("SpatialPositionBatch", TestKind::test, System::runSpatialPositionBatchTest),
        entry("SpatialPresetIntegrity", TestKind::test, System::runSpatialPresetIntegrityTest),
        entry("SpatialRoutingDifference", TestKind::test, System::runSpatialRoutingDifferenceTest),
        entry("SpatialRoutingBypassEquiv", TestKind::test, System::runSpatialRoutingBypassEquivalenceTest),
        entry("SpatialRoutingLatencyContract", TestKind::test, System::runSpatialRoutingLatencyContractTest),
        entry("SpatialRoutingBinauralImage", TestKind::test, System::runSpatialRoutingBinauralImageTest),
        entry("SpatialMetricBattery", TestKind::test, System::runSpatialMetricBatteryTest),
        entry("SpatialRoutingNoiseImage", TestKind::test, System::runSpatialRoutingNoiseImageTest),
        entry("SpatialRoutingModeSwitch", TestKind::test, System::runSpatialRoutingModeSwitchStabilityTest),
        entry("SpatialRoutingPresetRoundTrip", TestKind::test, System::runSpatialRoutingPresetRoundTripTest),
        entry("SpatialRoutingMonoSafety", TestKind::test, System::runSpatialRoutingMonoSafetyTest),
        entry("SpatialControlSource", TestKind::test, System::runSpatialControlSourceArbitrationTest),
        entry("SpatialMotionSource", TestKind::test, System::runSpatialMotionSourceTest),
        entry("SpatialMotionContinuity", TestKind::test, System::runSpatialMotionContinuityStressTest),
        entry("SpatialMotionFeatureCoverage", TestKind::test, System::runSpatialMotionFeatureCoverageTest),
        entry("OrbitAzimuthContinuity", TestKind::test, System::runOrbitAzimuthContinuityTest),
        entry("Figure8FrequencyRatio", TestKind::test, System::runFigure8FrequencyRatioTest),
        entry("BreatheDistanceOnly", TestKind::test, System::runBreatheDistanceOnlyTest),
        entry("DepthZeroStillness", TestKind::test, System::runDepthZeroStillnessTest),
        entry("ChoreographyOffsets", TestKind::test, System::runChoreographyOffsetsTest),
        entry("TapEnvelopeMeter", TestKind::test, System::runTapEnvelopeMeterTest),
        entry("PerTapSpatialization", TestKind::test, System::runPerTapSpatialization),
        entry("PerTapMonoSafety", TestKind::test, System::runPerTapMonoSafety),
        entry("PerTapMotionConstellation", TestKind::test, System::runPerTapMotionConstellation),
        entry("PerTapMotionSmoothness", TestKind::test, System::runPerTapMotionSmoothness),
        entry("IndependentStereoCaptureMatrix", TestKind::test, System::runIndependentStereoCaptureMatrixTest),
        entry("IndependentPostTreatmentParity", TestKind::test, System::runIndependentPostTreatmentParityTest),
        entry("IndependentEnvelopeAuthority", TestKind::test, System::runIndependentEnvelopeAuthorityTest),
        entry("IndependentToneAuthority", TestKind::test, System::runIndependentToneAuthorityTest),
        entry("IndependentSpatialExactOnce", TestKind::test, System::runIndependentSpatialExactOnceTest),
        entry("AllVoiceSpatialConfigPropagation", TestKind::test, System::runAllVoiceSpatialConfigPropagationTest),
        entry("IndependentVoiceHistoryReentry", TestKind::test, System::runIndependentVoiceHistoryReentryTest),
        entry("BinauralLatency", TestKind::test, BinauralProcessorTestAccess::runBinauralLatencySelfTest),
        entry("IndependentLevelParityProbe", TestKind::measurement, System::runIndependentLevelParityProbe),
        entry("SourceWidthEnergyConservation", TestKind::test, System::runSourceWidthEnergyConservationTest),
        entry("SpatialHeadViewPaintBench", TestKind::measurement, System::runSpatialHeadViewPaintBenchTest),
        // Processor, state, presets, routing and editor
        entry("ProcessorTempoMidiUpdate", TestKind::test,
              [](SpatialEchoAudioProcessor& processor) { return processor.runProcessorTempoMidiUpdateTest(); }),
        entry("DisplayNameRollout", TestKind::test, System::runDisplayNameRolloutTest),
        entry("UserVisibleStringUpdate", TestKind::test, System::runUserVisibleStringUpdateTest),
        entry("EditorCreationSmoke", TestKind::test, System::runEditorCreationSmokeTest),
        entry("EditorWindowZoom", TestKind::test, System::runEditorWindowZoomTest),
        entry("EditorKnobResetDefaults", TestKind::test, System::runEditorKnobResetDefaultsTest),
        entry("EditorSnapshot", TestKind::measurement, System::runEditorSnapshot),
        entry("OutputMixTextParsing", TestKind::test, System::runOutputMixTextParsingTest),
        entry("FreeHeadTimeReadout", TestKind::test, System::runFreeHeadTimeReadoutTest),
        entry("ComboBoxPopupsFit", TestKind::test, System::runComboBoxPopupsFitTest),
        entry("DrawerLookMatchesStandalone", TestKind::test, System::runDrawerLookMatchesStandaloneTest),
        entry("PresetBrowserKeyboardRouting", TestKind::test, System::runPresetBrowserKeyboardRoutingTest),
        entry("InteractiveWalkthrough", TestKind::test, System::runInteractiveWalkthroughTest),
        entry("WalkthroughCardClearsSurfaces", TestKind::test, System::runWalkthroughCardClearsSurfacesTest),
        entry("ParameterValueNoTruncation", TestKind::test, System::runParameterValueNoTruncationTest),
        entry("SerializationRoundTrip", TestKind::test,
              [](SpatialEchoAudioProcessor& processor) { return processor.runSerializationRoundTripTest(); }),
        entry("NoAllocations", TestKind::test,
              [](SpatialEchoAudioProcessor& processor) { return processor.runNoAllocationsTest(); }),
        entry("DenormalHandling", TestKind::test, System::runDenormalHandlingTest),
        entry("InvalidInputRecovery", TestKind::test, System::runInvalidInputRecoveryTest),
        entry("BrandSealEmbedded", TestKind::test, System::runBrandSealEmbeddedTest),
        entry("OutputProtection", TestKind::test, System::runOutputProtectionTest),
        entry("OutputLatencyContract", TestKind::test, System::runOutputLatencyContractTest),
        entry("StateSchemaBoundary", TestKind::test, System::runStateSchemaBoundaryTest),
        entry("ReleaseAccessibility", TestKind::test, System::runReleaseAccessibilityTest),
        entry("AutoPanic", TestKind::test, System::runAutoPanicTest),
        entry("OutputGainAutomationSmoothing", TestKind::test, System::runOutputGainAutomationSmoothingTest),
        entry("ReverbNoiseBlockRateIndependence", TestKind::test, System::runReverbNoiseBlockRateIndependenceTest),
        entry("ReverbRoutingImpulseContracts", TestKind::test, System::runReverbRoutingImpulseContractsTest),
        entry("ReverbRoutingHeadroomMatrix", TestKind::test, System::runReverbRoutingHeadroomMatrixTest),
        entry("ActiveReverbRoutingSwitchContinuity", TestKind::test,
              System::runActiveReverbRoutingSwitchContinuityTest),
        entry("FactoryPresetCategoryIntegrity", TestKind::test, System::runFactoryPresetCategoryIntegrityTest),
        entry("FactoryPresetDescriptionCoverage", TestKind::test, System::runFactoryPresetDescriptionCoverageTest),
        entry("FactoryPresetLoad", TestKind::test, System::runFactoryPresetLoadTest),
        entry("PresetLoadUndoRecovery", TestKind::test, System::runPresetLoadUndoRecoveryTest),
        entry("FactoryPresetNoisePolicy", TestKind::test, System::runFactoryPresetNoisePolicyTest),
        entry("TapePresetNoiseEnablePolicy", TestKind::test, System::runTapePresetNoiseEnablePolicyTest),
        entry("UserPresetPersistence", TestKind::test, System::runUserPresetPersistenceTest),
        entry("UserPresetSofaPathPolicy", TestKind::test, System::runUserPresetSofaPathPolicyTest),
        entry("MixLockStateRecallAuthority", TestKind::test, System::runMixLockStateRecallAuthorityTest),
        entry("SelectorSideEffectStoppedTransportRoundTrip", TestKind::test,
              System::runSelectorSideEffectStoppedTransportRoundTripTest),
        entry("WholeStateReplacementSuspension", TestKind::test, System::runWholeStateReplacementSuspensionTest),
        entry("HostTailLengthContract", TestKind::test, System::runHostTailLengthContractTest),
        entry("ActiveFactoryPresetSwitchContinuity", TestKind::test,
              System::runActiveFactoryPresetSwitchContinuityTest),
        entry("StartupDefaultStateConsistency", TestKind::test, System::runStartupDefaultStateConsistencyTest),
        entry("StartupDefaultNoiseFloor", TestKind::test, System::runStartupDefaultNoiseFloorTest),
        entry("InitDefaultParameterContract", TestKind::test, System::runInitDefaultParameterContractTest),
        entry("PreampPathCoverage", TestKind::test, System::runPreampPathCoverageTest),
        entry("ToneStackIsolation", TestKind::test, System::runToneStackIsolationTest),
        entry("MixSafety", TestKind::test, System::runMixSafetyTest),
        entry("ActivePathMixMatrix", TestKind::test, System::runActivePathMixMatrixTest),
        entry("BranchTiltModeSwitchContinuity", TestKind::test, System::runBranchTiltModeSwitchContinuityTest),
        entry("DualPathBalanceDefault", TestKind::test, System::runDualPathBalanceDefaultTest),
        entry("BlockSizeExtremes", TestKind::test, System::runBlockSizeExtremesTest),
        entry("CpuStressProfile", TestKind::measurement, System::runCpuStressProfileTest),
        entry("DropdownWindowPlacementBounds", TestKind::test, System::runDropdownWindowPlacementBoundsTest),
        entry("ReverbPresetModeChoiceWrite", TestKind::test, System::runReverbPresetModeChoiceWriteTest),
        entry("ReverbPresetModeChoiceWriteTailRetention", TestKind::test,
              System::runReverbPresetModeChoiceWriteTailRetentionTest),
        entry("OutputProtectionToggleContinuity", TestKind::test, System::runOutputProtectionToggleContinuityTest),
        entry("NoiseRoutingTimeline", TestKind::test, System::runNoiseRoutingTimelineTest),
        entry("SerialRoutingHeadroomProbe", TestKind::measurement, System::runSerialRoutingHeadroomProbe),
        entry("ReverbRouteReturnParity", TestKind::measurement, System::runReverbRouteReturnParity),
        entry("PresetRosterRenders", TestKind::fixture, System::runPresetRosterRenders),
        entry("ReverbMacroSmoothingFlutterProbe", TestKind::measurement, System::runReverbMacroSmoothingFlutterProbe),
        entry("ReverbEngineLevelParityProbe", TestKind::measurement, System::runReverbEngineLevelParityProbe),
        entry("DropdownWindowHostOwnsChildren", TestKind::test, System::runDropdownWindowHostOwnsChildrenTest),
        entry("EqPathPlacement", TestKind::test, System::runEqPathPlacementTest),
        entry("PresetCompatibility", TestKind::test,
              [](SpatialEchoAudioProcessor& processor) { return processor.runPresetCompatibilityTest(); }),
        entry("ProcessorLifecycleTeardown", TestKind::test, System::runProcessorLifecycleTeardownTest),
        entry("DefaultPresetAutoLoad", TestKind::test, System::runDefaultPresetAutoLoadTest),
        entry("PresetLoadSilenceRate", TestKind::measurement, System::runPresetLoadSilenceRate),
    };
    return tests;
}

const TestEntry* findTest(const juce::String& name)
{
    for (const auto& candidate : registry())
        if (name == candidate.name)
            return &candidate;
    return nullptr;
}

const char* kindName(TestKind kind)
{
    switch (kind)
    {
        case TestKind::test:
            return "test";
        case TestKind::measurement:
            return "measurement";
        case TestKind::fixture:
            return "fixture";
    }
    return "test";
}

juce::String runTest(const TestEntry& test)
{
    try
    {
        if (test.standalone != nullptr)
            return test.standalone();

        SpatialEchoAudioProcessor processor;
        processor.prepareToPlay(48000.0, 512);
        return test.withProcessor(processor);
    }
    catch (const std::exception& e)
    {
        return "[ERROR] " + juce::String(test.name) + " threw: " + juce::String(e.what());
    }
    catch (...)
    {
        return "[ERROR] " + juce::String(test.name) + " threw an unknown exception";
    }
}
}  // namespace SpatialEchoTests::Headless

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
