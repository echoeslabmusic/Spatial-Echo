// SpatialEchoAudioProcessor: Main audio plugin processor header: parameters, DSP graph, preset/undo management,
//   binaural access.
// Realtime-safety: No allocation/locks in processBlock(); parameter updates deferred via APVTS tree listener.

#pragma once

#include <JuceHeader.h>
#include <memory>
#include <array>
#include "DSP/EchoesModel.h"
#include "DSP/OutputProtectionStage.h"
#include "ScopeDataFifo.h"
#include "SpatialParamIDs.h"

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#include "Tests/System/EchoesModelTestAccess.h"
#endif

using namespace juce;

//==============================================================================
/**
 */
class SpatialEchoAudioProcessor : public juce::AudioProcessor,
                                  public juce::ValueTree::Listener,
                                  private juce::Timer
#if JucePlugin_Enable_ARA
    ,
                                  public juce::AudioProcessorARAExtension
#endif
{
public:
    //==============================================================================
    SpatialEchoAudioProcessor();
    ~SpatialEchoAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // Access to parameters for the editor
    AudioProcessorValueTreeState& getValueTreeState() { return treestate; }
    juce::UndoManager& getUndoManager() { return undoManager; }
    void flushPendingHostUpdatesForUi();
    // Non-audio writers bracket one complete position edit. Nested scopes are
    // allowed (preset -> defaults, ALL-head edit -> individual tap callbacks).
    class ScopedSpatialParameterBatch
    {
    public:
        explicit ScopedSpatialParameterBatch(SpatialEchoAudioProcessor&);
        ~ScopedSpatialParameterBatch();
        ScopedSpatialParameterBatch(const ScopedSpatialParameterBatch&) = delete;
        ScopedSpatialParameterBatch& operator=(const ScopedSpatialParameterBatch&) = delete;

    private:
        SpatialEchoAudioProcessor& processor;
        juce::ScopedLock lock;
    };

    struct SpatialPositionState
    {
        TapSpatialParams taps;
        float azimuth = 0.0f, elevation = 0.0f, distance = 1.0f;
    };
    BinauralProcessor* getBinauralProcessor() { return echomodel.getBinauralProcessor(); }
    const BinauralProcessor* getBinauralProcessor() const { return echomodel.getBinauralProcessor(); }
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    SpatialField* getSpatialFieldForTest() { return echomodel.getSpatialField(); }
    SpatialPositionState readSpatialPositionsForTest() { return readSpatialPositionsForAudio(); }
#endif
    ScopeDataFifo& getScopeDataFifo() { return scopeDataFifo; }

    // Free-running base delay in ms for a RepeatRate value (0..1): the time the
    // tape delay uses and the Rate control displays.
    static float freeRunningBaseDelayMs(float repeatRate) noexcept;

    // Spatial motion division table and the motion state the editor previews.
    static int getMotionNoteDivisionCount() { return SpatialIDs::motion::kDivisionMenuCount; }
    static float getMotionNoteDivisionMultiplier(int divisionIndex);
    double getSpatialMotionPreviewPhaseCycles() const
    {
        return spatialMotionPreviewPhaseCyclesForUi.load(std::memory_order_relaxed);
    }
    float getSpatialMotionPreviewRateHz() const { return spatialMotionRateHzForUi.load(std::memory_order_relaxed); }
    float getSpatialMotionPreviewBlockDurationSec() const
    {
        return spatialMotionBlockDurationSecForUi.load(std::memory_order_relaxed);
    }
    float getSpatialMotionPreviewModAz() const { return spatialMotionModAzForUi.load(std::memory_order_relaxed); }
    float getSpatialMotionPreviewModEl() const { return spatialMotionModElForUi.load(std::memory_order_relaxed); }
    float getSpatialMotionPreviewModDist() const { return spatialMotionModDistForUi.load(std::memory_order_relaxed); }
    int getSpatialMotionPreviewPatternId() const
    {
        return spatialMotionPreviewPatternIdForUi.load(std::memory_order_relaxed);
    }
    float getSpatialMotionPreviewDepth01() const
    {
        return spatialMotionPreviewDepth01ForUi.load(std::memory_order_relaxed);
    }
    float getEchoBranchMeterLevel() const { return echomodel.getEchoBranchMeterLevel(); }
    float getReverbBranchMeterLevel() const { return echomodel.getReverbBranchMeterLevel(); }
    // Per-tap envelope levels (peak-envelope of delay head tap outputs). tapIndex 0..2; out-of-range returns 0.
    float getTapEnvelopeLevel(int tapIndex) const { return echomodel.getTapEnvelopeLevel(tapIndex); }
    // Self-oscillation heat for the PANIC light: ~1.0 = tape saturator knee (audible
    // grit), higher = grosser distortion. Read by the editor to fade green->amber->red.
    float getSelfOscillationHeat() const { return echomodel.getSelfOscHeat(); }
    float getAutoPanicRunawayMonitorNormalization() const
    {
        return echomodel.getAutoPanicRunawayMonitorNormalization();
    }

    // ==== Auto-panic watchdog (AutoPanic parameter) ====
    // Watches the post-model bus (processingBuffer, before OutputGain and
    // OutputProtection) once per block and fires the same flush a manual PANIC
    // would, but only on states no musical setting can produce:
    //  - non-finite: any NaN/Inf on the bus -> immediate hard flush. Without
    //    it the wet chain stays silently wedged forever (the output stage
    //    scrubs NaN to 0, masking the corruption).
    //  - runaway: the watched bus peak, normalized back to its pre-
    //    presentation level, exceeds kAutoPanicPeakThreshold AND is at least
    //    kAutoPanicInputRatio above the input peak, sustained for
    //    kAutoPanicHoldSeconds -> fade-out flush. This keeps public wet
    //    loudness trim from masquerading as loop instability while still
    //    catching actual self-growth and downstream finite corruption.
    static constexpr float kAutoPanicPeakThreshold = 8.0f;  // ~ +18 dBFS
    static constexpr float kAutoPanicInputRatio = 31.6f;    // ~ +30 dB above input
    static constexpr float kAutoPanicHoldSeconds = 0.4f;
    static constexpr float kAutoPanicCooldownSeconds = 2.0f;
    // Input samples beyond +36 dBFS are clamped and NaN/Inf zeroed on entry.
    static constexpr float kMaxInputMagnitude = 64.0f;
    // Total auto-panic triggers since construction (UI flash + tests).
    uint32_t getAutoPanicEventCount() const noexcept { return autoPanicEvents.load(std::memory_order_relaxed); }

    // Tempo sync calculation (public for debug testing)
    float getCalculatedSyncedDelayMs(double bpm, int divisionIndex) const
    {
        return calculateSyncedDelayMs(bpm, divisionIndex);
    }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    static float debugMapFeedbackToLoopGain(float raw) noexcept;
    float debugGetNoiseFeedbackIntensity() const noexcept
    {
        return EchoesModelTestAccess::noiseFeedbackIntensity(echomodel);
    }

    bool debugGetEchoPathPreTrimMetrics(float& peak, float& rms) const noexcept
    {
        return EchoesModelTestAccess::getEchoPathPreTrimMetrics(echomodel, peak, rms);
    }

    void debugEnableEchoPathPreTrimProbe(bool enabled) noexcept
    {
        EchoesModelTestAccess::enableEchoPathPreTrimProbe(echomodel, enabled);
    }

    void debugResetEchoPathPreTrimProbe() noexcept { EchoesModelTestAccess::resetEchoPathPreTrimProbe(echomodel); }

    void debugEnableReverbInputProbe(bool enabled) noexcept
    {
        EchoesModelTestAccess::enableReverbInputProbe(echomodel, enabled);
    }

    void debugResetReverbInputProbe() noexcept { EchoesModelTestAccess::resetReverbInputProbe(echomodel); }

    bool debugGetReverbInputMetrics(int routingMode, float& peak, float& rms) const noexcept
    {
        return EchoesModelTestAccess::getReverbInputMetrics(echomodel, routingMode, peak, rms);
    }

    void debugEnableReverbReturnProbe(bool enabled) noexcept
    {
        EchoesModelTestAccess::enableReverbReturnProbe(echomodel, enabled);
    }

    void debugResetReverbReturnProbe() noexcept { EchoesModelTestAccess::resetReverbReturnProbe(echomodel); }

    bool debugGetReverbReturnMetrics(int routingMode, float& peak, float& rms) const noexcept
    {
        return EchoesModelTestAccess::getReverbReturnMetrics(echomodel, routingMode, peak, rms);
    }

    bool debugCopyReverbReturnBuffer(std::vector<std::vector<float>>& out, int numSamples) const
    {
        return EchoesModelTestAccess::copyReverbReturnBuffer(echomodel, out, numSamples);
    }

    float debugGetFeedbackCompensationGain() const noexcept
    {
        return EchoesModelTestAccess::feedbackCompensationGain(echomodel);
    }

    void enableStageTimingCapture(bool enabled);
    void resetStageTimingCapture();
    bool getStageTimingSnapshot(EchoesModel::StageTimingSnapshot& out) const;
    // Auto-panic test fault injectors (see EchoesModelTestAccess).
    void debugPoisonTapeLoop(float value) { EchoesModelTestAccess::poisonTapeLoop(echomodel, value); }
    void debugSetWetBusFault(float amplitude) { EchoesModelTestAccess::setWetBusFault(echomodel, amplitude); }
#endif

    // Simple preset management
    juce::StringArray getFactoryPresetNames() const;
    bool factoryPresetExists(const juce::String& name) const;
    void loadFactoryPreset(const juce::String& name);
    juce::StringArray getUserPresetNames() const;
    juce::File getUserPresetDirectoryOnDisk() const;
    bool userPresetExists(const juce::String& name) const;
    bool saveUserPreset(const juce::String& name);
    void loadUserPreset(const juce::String& name);
    bool deleteUserPreset(const juce::String& name);
    bool renameUserPreset(const juce::String& oldName, const juce::String& newName);
    juce::String getPresetNameFromFile(const juce::File& file) const;
    bool importUserPresetFromFile(const juce::File& file, juce::String* outName = nullptr);
    bool exportUserPresetToFile(const juce::File& file, const juce::String& name);
    void loadDefaultPreset();

    // This function will alert the listener of parameter changes
    void valueTreePropertyChanged(juce::ValueTree& treeWhosePropertyHasChanged,
                                  const juce::Identifier& property) override;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Headless test entry points, defined in PluginProcessorDebug.cpp and
    // registered in Tests/Headless/HeadlessTestRunner.cpp.
    juce::String runProcessorTempoMidiUpdateTest();  // Regression: host BPM + MIDI CC updates without ValueTree changes
    juce::String runSerializationRoundTripTest();    // Verify save/load produces identical audio
    juce::String runPresetCompatibilityTest();       // Load unknown params without breaking state
    juce::String runNoAllocationsTest();             // Detect allocations during audio processing
#endif

private:
    void timerCallback() override;
    void flushPendingHostParameterUpdates();
    void queuePendingFloatHostUpdate(std::atomic<float>& pendingValue, uint32_t maskBit, float value);
    void queuePendingChoiceHostUpdate(std::atomic<int>& pendingIndex, uint32_t maskBit, int index);
    void applyHostFloatParameterUpdate(const char* paramId, float value);
    void applyHostChoiceParameterUpdate(const char* paramId, int index);

    enum : uint32_t
    {
        pendingBinauralAzimuthMask = 1u << 0,
        pendingBinauralElevationMask = 1u << 1,
        pendingBinauralDistanceMask = 1u << 2,
        pendingPreampPresetMask = 1u << 3,
        pendingInputLevelMask = 1u << 4,
        pendingBassMask = 1u << 5,
        pendingTrebleMask = 1u << 6
    };

    std::atomic<uint32_t> pendingHostParamUpdateMask{0u};
    // Serializes non-audio snapshot/flush consumers. The audio callback only
    // publishes atomics and never acquires this lock.
    juce::CriticalSection stateSnapshotLock;
    std::atomic<float> pendingBinauralAzimuth{0.0f};
    std::atomic<float> pendingBinauralElevation{0.0f};
    std::atomic<float> pendingBinauralDistance{1.0f};
    std::atomic<int> pendingPreampPresetIndex{0};
    std::atomic<float> pendingInputLevel{0.5f};
    std::atomic<float> pendingBass{0.5f};
    std::atomic<float> pendingTreble{0.5f};

    // ------ Parameter storage variables

    std::atomic<float>*InputLevel = nullptr, *Bass = nullptr, *Treble = nullptr, *RepeatRate = nullptr,
    *Intensity = nullptr, *EchoSend = nullptr, *Panic = nullptr;
    std::atomic<float>*Mix = nullptr, *EchoLevel = nullptr, *ReverbLevel = nullptr, *MixLock = nullptr,
    *OutputGain = nullptr, *OutputProtection = nullptr, *FeedbackCompensation = nullptr;
    std::atomic<float>* AutoPanic = nullptr;

    // Auto-panic watchdog state (audio thread only, except the event counter)
    void runAutoPanicWatchdog(const juce::AudioBuffer<float>& inputBuffer, int numSamples, int numChannels);
    int autoPanicHeldSamples = 0;      // consecutive samples the runaway condition has held
    int autoPanicCooldownSamples = 0;  // detector disarm window after a trigger
    std::atomic<uint32_t> autoPanicEvents{0};

    // Tempo sync parameters
    std::atomic<float>* TempoSync = nullptr;
    AudioParameterChoice* Head1NoteDivision = nullptr;
    AudioParameterChoice* Head2NoteDivision = nullptr;
    AudioParameterChoice* Head3NoteDivision = nullptr;

    // Head ratio preset: 0=Classic 1:1.9:2.75, 1=Idealized 1:2:3, 2=Golden, 3=Linear
    AudioParameterChoice* HeadRatioPreset = nullptr;

    // Noise / wow-flutter: per-flavor internal values, not host parameters.

    // Power switch state (detect 1->0 transition to trigger kill)
    float lastKillValue = 1.0f;

    // Power fade envelope (smooth fade to silence when power turns OFF)
    float powerFadeEnvelope = 1.0f;  // 1.0 = full volume, 0.0 = silent
    bool powerFading = false;
    static constexpr float POWER_FADE_TIME_MS = 20.0f;  // 20ms fade

    // Mix lock state: captures and holds the current mix value while locked.
    bool mixLockWasEngaged = false;
    float mixLockHeldValue = 0.5f;

    AudioParameterChoice* DelaySetting = nullptr;
    std::atomic<float>* PingPong = nullptr;  // false=Normal, true=Ping-Pong

    // Reverb engine selection
    AudioParameterChoice* ReverbEngineParam = nullptr;  // 0 = Spring, 1 = Cloud
    AudioParameterChoice* ReverbRouting = nullptr;      // 0 = Parallel, 1 = Echo->Reverb, 2 = Reverb->Echo

    // Reverb macro schema: engine-native params
    // are derived inside ReverbProcessor from these seven macros.
    std::atomic<float>* ReverbSize = nullptr;
    std::atomic<float>* ReverbDecay = nullptr;
    std::atomic<float>* ReverbTone = nullptr;        // dark↔bright macro
    std::atomic<float>* ReverbMod = nullptr;         // modulation depth macro
    std::atomic<float>* ReverbCharacter = nullptr;   // per-engine character macro
    std::atomic<float>* ReverbPreDelay = nullptr;    // Pre-delay for spatial depth (0-1 = 0-200ms)
    std::atomic<float>* ReverbWidth = nullptr;       // 0 = mono … 1 = wide
    std::atomic<float>* ReverbDuckAmount = nullptr;  // Dry-input ducking applied to reverb wet branch
    // Binaural parameters
    std::atomic<float>* BinauralEnabled = nullptr;
    std::atomic<float>* BinauralAzimuth = nullptr;
    std::atomic<float>* BinauralElevation = nullptr;
    std::atomic<float>* BinauralDistance = nullptr;
    std::atomic<float>* BinauralWetMix = nullptr;
    std::atomic<float>* BinauralCrossover = nullptr;
    juce::AudioParameterChoice* BinauralQuality = nullptr;
    juce::AudioParameterChoice* SpatialRouting = nullptr;  // 0 = After Effects (default), 1 = Before Effects
    // Pattern-first motion schema plus append-only long-division extensions.
    juce::AudioParameterChoice* MotionPattern = nullptr;       // Still, Orbit, Pendulum, Figure8, Breathe, Wander
    std::atomic<float>* MotionRate = nullptr;                  // 0.01..8 Hz, log-skewed
    std::atomic<float>* MotionSync = nullptr;                  // 0=free, 1=tempo-synced
    juce::AudioParameterChoice* MotionDivision = nullptr;      // Original 17-choice automation domain
    juce::AudioParameterChoice* MotionLongDivision = nullptr;  // Off, 2/1, 4/1, 8/1, 16/1
    juce::AudioParameterChoice* MotionLongVariantDivision = nullptr;  // Off + long triplet/dotted periods
    std::atomic<float>* MotionDepth = nullptr;                        // 0..100%
    juce::AudioParameterChoice* MotionRetrig = nullptr;               // Free, Transport, Note, Random
    juce::AudioParameterChoice* MotionChoreo = nullptr;               // Unison, Constellation, Cascade
    std::atomic<float>* MotionSpread = nullptr;                       // 0..100% (choreography spread)
    double spatialMotionPhaseCycles = 0.0;
    int spatialMotionRandomStep = -1;
    double spatialMotionRandomPhaseOffsetCycles = 0.0;
    bool spatialMotionRandomPhasePrimed = false;
    bool spatialMotionNoteOnThisBlock = false;
    bool spatialMotionSmootherPrimed = false;
    float spatialMotionSmoothedAz = 0.0f;
    float spatialMotionSmoothedEl = 0.0f;
    float spatialMotionSmoothedDist = 0.0f;
    // Per-tap motion constellation smoothers (only used when spatialMotionEnabled && positionMode==1)
    std::array<float, 3> tapMotionSmoothedAz = {};
    std::array<float, 3> tapMotionSmoothedEl = {};
    std::array<float, 3> tapMotionSmoothedDist = {};
    bool tapMotionSmootherPrimed = false;
    std::atomic<double> spatialMotionPreviewPhaseCyclesForUi{0.0};
    std::atomic<float> spatialMotionRateHzForUi{0.35f};
    std::atomic<float> spatialMotionBlockDurationSecForUi{0.0f};
    std::atomic<float> spatialMotionModAzForUi{0.0f};
    std::atomic<float> spatialMotionModElForUi{0.0f};
    std::atomic<float> spatialMotionModDistForUi{0.0f};
    std::atomic<int> spatialMotionPreviewPatternIdForUi{0};     // Index into motion::kPatternChoices
    std::atomic<float> spatialMotionPreviewDepth01ForUi{0.0f};  // 0..1

    // Advanced binaural parameters
    std::atomic<float>* BinauralSourceWidth = nullptr;  // Source width 0-100%
    // Per-tap spatial positioning parameters
    juce::AudioParameterChoice* TapPositionMode = nullptr;  // 0=Linked, 1=Independent
    std::atomic<float>* Head1Azimuth = nullptr;
    std::atomic<float>* Head1Elevation = nullptr;
    std::atomic<float>* Head1Distance = nullptr;
    std::atomic<float>* Head2Azimuth = nullptr;
    std::atomic<float>* Head2Elevation = nullptr;
    std::atomic<float>* Head2Distance = nullptr;
    std::atomic<float>* Head3Azimuth = nullptr;
    std::atomic<float>* Head3Elevation = nullptr;
    std::atomic<float>* Head3Distance = nullptr;
    // Tape surface: flavor + drive with one master noise override. The resolved
    // per-flavor bundle values below are plain caches written by
    // updateParameters() and read by the same call path.
    std::atomic<float>* TapeDrive = nullptr;
    std::atomic<float>* TapeNoiseEnabled = nullptr;
    juce::AudioParameterChoice* TapeFlavorParam = nullptr;  // CLEAN/WARM/HOT/CASSETTE/WORN
    float tapePresetAsperityOverride = -1.0f;
    float tapePresetDropoutOverride = -1.0f;
    float tapePresetScrapeScale = 1.0f;
    float tapePresetLpfScale = 1.0f;

    // Preamp presets
    juce::AudioParameterChoice* PreampPresetParam = nullptr;
    std::atomic<float>* PreampPresetValue = nullptr;
    std::atomic<int> lastPreampPresetIndex{-1};
    bool preampPresetApplyInProgress = false;
    bool lastPreampParamsValid = false;
    float lastPreampInput = 0.5f;
    float lastPreampBass = 0.5f;
    float lastPreampTreble = 0.5f;

    // =========================================================================
    // Spatial Control Source (MIDI / Manual / Automation / Motion)
    // =========================================================================
public:
    enum class SpatialControlSource
    {
        Manual,
        MIDI,
        Automation,
        Motion
    };
    SpatialControlSource getSpatialControlSource() const { return spatialControlSource.load(); }
    void noteSpatialManualInteraction();

private:
    std::atomic<SpatialControlSource> spatialControlSource{SpatialControlSource::Manual};
    static constexpr double kManualSpatialActivityWindowSec = 0.4;
    std::atomic<bool> manualSpatialActivityRequested{false};
    int manualSpatialActivitySamplesRemaining = 0;
    bool spatialSourceTrackingInitialized = false;
    float lastObservedSpatialAzimuth = 0.0f;
    float lastObservedSpatialElevation = 0.0f;
    float lastObservedSpatialDistance = 1.0f;

    // Fixed MIDI CC numbers for spatial position control. MIDI reaches the
    // processor only in the Standalone build (see processSpatialMidiCC).
    static constexpr int kMidiCCAzimuth = 21;
    static constexpr int kMidiCCElevation = 22;
    static constexpr int kMidiCCDistance = 23;

    // Process incoming MIDI CC for spatial control
    void processSpatialMidiCC(const juce::MidiBuffer& midiMessages);

    // Sample rate from the last prepareToPlay; time-based audio-thread
    // calculations fall back to it when the host reports no rate.
    double lastSampleRate = 44100.0;

    // RT-safe processing buffer (preallocated to avoid audio-thread allocations)
    std::vector<std::vector<float>> processingBuffer;

    OutputProtectionStage outputProtectionStage;

    // Host bypass crossfade: a host flipping bypass switches between
    // processBlock and processBlockBypassed. Keep processing for a short ramp
    // and blend toward the latency-aligned dry signal so the switch is click-free.
    void processActiveBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&);
    void applyBypassRamp(juce::AudioBuffer<float>&) noexcept;
    static constexpr double kBypassRampTimeSec = 0.020;
    static constexpr float kBypassRampSettleTolerance = 1.0e-6f;
    std::vector<std::vector<float>> bypassDryScratch;  // delayed dry per block, preallocated
    float bypassRampGain = 0.0f;                       // 0 = processed, 1 = bypassed
    float bypassRampTarget = 0.0f;
    float bypassRampStep = 1.0f;
    bool lastBlockWasBypassed = false;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGainSmoother{1.0f};
    bool outputGainSmootherPrepared = false;
    static constexpr double kOutputGainSmoothingTimeSec = 0.020;
    // Output Protection is a signal-path switch, not a gain: flipping it swaps an
    // oversampled nonlinear path for a straight one. Crossfading between the two
    // keeps the toggle click-free. Linear (not Multiplicative) because this ramp
    // must be able to reach exactly 0.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outputProtectionMixSmoother{1.0f};
    bool outputProtectionSmootherPrepared = false;
    static constexpr double kOutputProtectionSmoothingTimeSec = 0.020;

    // ------ Plugin Objects

    // Scope FIFO (audio thread pushes, UI thread reads)
    ScopeDataFifo scopeDataFifo;
    std::vector<float> scopeMixBuf;  // Scratch buffer for mono mix-down (pre-allocated in prepareToPlay)

    // Space echo model
    EchoesModel echomodel;

    // Undo manager (must outlive treestate)
    juce::UndoManager undoManager;

    // Valuetree objects and functions
    AudioProcessorValueTreeState treestate;
    juce::ValueTree defaultStateSnapshot;
    AudioProcessorValueTreeState::ParameterLayout params();

    // -------

    // Functions for getting params from treestate and applying to DSP objects
    void populateParameters();
    void syncPresetTrackingFromState();
    void updateParameters(int blockSamples);
    SpatialPositionState readSpatialPositionsForAudio();
    juce::CriticalSection spatialWriterLock;  // never acquired by audio
    unsigned spatialWriterDepth = 0;          // guarded by spatialWriterLock
    std::atomic<uint64_t> spatialPositionGeneration{0};
    SpatialPositionState lastCompleteSpatialPositions;  // audio-owned
    void reloadDefaultHrtf();
    void loadDefaultPresetInternal(bool reloadHrtf);
    juce::ValueTree createStateSnapshot();

    // Parameter thread safety flag
    std::atomic_flag changesApplied;

    // Current multi tap playhead state
    int playheadstates[3] = {0, 0, 0};

    // Host tempo tracking with hysteresis
    double hostBpm = 120.0;
    double lastValidBpm = 120.0;          // Cached BPM for standalone/invalid host
    double bpmHysteresisThreshold = 0.5;  // Ignore BPM changes < 0.5

    // PPQ position tracking for beat-accurate sync
    double hostPpqPosition = 0.0;
    bool hostIsPlaying = false;

    // Note division multipliers (relative to quarter note)
    // Index matches NoteDivision parameter choice
    // Organized: Straight notes, then Triplets, then Dotted
    static constexpr float noteDivisionMultipliers[] = {
        // Straight notes
        4.0f,     // 0: 1/1 (whole note)
        2.0f,     // 1: 1/2 (half note)
        1.0f,     // 2: 1/4 (quarter note)
        0.5f,     // 3: 1/8 (eighth note)
        0.25f,    // 4: 1/16 (sixteenth note)
        0.125f,   // 5: 1/32 (thirty-second note)
        0.0625f,  // 6: 1/64 (sixty-fourth note)
        // Triplets (2/3 of the straight note duration)
        1.3333f,  // 7: 1/2T (half triplet)
        0.6667f,  // 8: 1/4T (quarter triplet)
        0.3333f,  // 9: 1/8T (eighth triplet)
        0.1667f,  // 10: 1/16T (sixteenth triplet)
        0.0833f,  // 11: 1/32T (thirty-second triplet)
        // Dotted (1.5x the straight note duration)
        3.0f,    // 12: 1/2D (dotted half)
        1.5f,    // 13: 1/4D (dotted quarter)
        0.75f,   // 14: 1/8D (dotted eighth)
        0.375f,  // 15: 1/16D (dotted sixteenth)
        0.1875f  // 16: 1/32D (dotted thirty-second)
    };

    // Motion exposes straight long periods through SpatialLongDivision and
    // triplet/dotted long periods through SpatialLongVariantDivision.
    // Combined-menu indices 17-28 map to these entries.
    static constexpr float slowMotionDivisionMultipliers[] = {
        8.0f, 16.0f, 32.0f, 64.0f, 16.0f / 3.0f, 32.0f / 3.0f, 64.0f / 3.0f, 128.0f / 3.0f, 12.0f, 24.0f, 48.0f, 96.0f};

    // Calculate delay time in ms from tempo and note division
    float calculateSyncedDelayMs(double bpm, int divisionIndex) const
    {
        if (bpm <= 0.0)
            bpm = 120.0;
        if (divisionIndex < 0 || divisionIndex > 16)
            divisionIndex = 3;  // default 1/8

        // Quarter note duration in ms = 60000 / BPM
        float quarterNoteMs = 60000.0f / static_cast<float>(bpm);

        // Apply note division multiplier
        return quarterNoteMs * noteDivisionMultipliers[divisionIndex];
    }

    // Design: Mode tables are classic defaults.
    // Early modes = echo only, mid modes = echo+reverb, last mode = reverb only.
    // Changes are allowed if intentional; update docs/tests accordingly.
    int reverbEnabled[12] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
    int delayEnabled[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};

    // Function to apply playhead states based on mode selection
    void applyDelaySettings();

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatialEchoAudioProcessor)
};
