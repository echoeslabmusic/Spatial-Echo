// EchoesModel: Top-level signal routing and parameter coordination.
// Realtime-safety: No allocation/locks in process path; smoothers for parameter updates

#pragma once

#include "ToneFiltering.h"
#include "TapeDelay.h"
#include "Reverbprocessor.h"
#include "PreampProcessor.h"
#include "BinauralProcessor.h"
#include "SpatialField.h"
#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

// Per-tap spatial positioning parameters
struct TapSpatialParams
{
    int positionMode = 0;  // 0=Linked (follow global binaural), 1=Independent (per-tap positions)

    // Per-tap positions (used only in Independent mode)
    float h1Azimuth = 0.0f;    // Head 1 azimuth (-180..180 deg)
    float h1Elevation = 0.0f;  // Head 1 elevation (-90..90 deg)
    float h1Distance = 1.0f;   // Head 1 distance (0.1..10 m)

    float h2Azimuth = 0.0f;    // Head 2 azimuth
    float h2Elevation = 0.0f;  // Head 2 elevation
    float h2Distance = 1.0f;   // Head 2 distance

    float h3Azimuth = 0.0f;    // Head 3 azimuth
    float h3Elevation = 0.0f;  // Head 3 elevation
    float h3Distance = 1.0f;   // Head 3 distance
};

// Per-tap motion constellation parameters
struct MotionTapParams
{
    float spread = 0.0f;  // Per-tap LFO phase offset spread: 0..100% (0=all taps unison, 100%=evenly distributed)
    int mode = 0;  // 0=Unison, 1=Constellation (fixed 120° offsets), 2=Cascade (offsets follow head delay ratios)
};

// Helper: calculate per-tap phase offset based on mode and spread
// Wired to SpatialEchoAudioProcessor::updateParameters (post-global motion block).
// Computes LFO phase offsets for per-tap motion constellation effects in Independent mode.
// Offsets scale with MotionTapSpread; applied via per-tap smoothed LFO evaluation.
inline float calculateTapPhaseOffset(int tapIndex, const MotionTapParams& tapMotion)
{
    if (tapMotion.mode == 0 || tapMotion.spread < 0.001f)
        return 0.0f;  // Unison: no offset

    if (tapMotion.mode == 1)  // Constellation: 0°, 120°, 240°
        return (tapIndex == 0) ? 0.0f : (tapIndex == 1) ? (120.0f / 360.0f) : (240.0f / 360.0f);

    if (tapMotion.mode == 2)  // Cascade: offsets follow head delay ratio pattern (0.33, 0.67 for typical ratios)
    {
        // Typical head ratios are 1:0.67:0.33; normalized to phase offsets
        constexpr float tapRatios[] = {0.0f, 0.33f, 0.67f};
        return tapRatios[tapIndex];
    }

    return 0.0f;
}

class EchoesModel
{
public:
    EchoesModel();
    ~EchoesModel();

    // The delay owns a first-party per-sample record saturator (8x at <=48kHz,
    // 4x above) inside its feedback write path.
    void Reset(float sampleRate, int numChannels, int maxBlockSize = 512);
    // Reverb is controlled through the consolidated macro schema
    // (ReverbMacroParams); all engine-native
    // parameters are derived inside ReverbProcessor.
    void UpdateParameters(float low, float high, float intensity, float delaytime, int playheadenabled[],
                          int delayenabled, float mix, int reverbenabled, float inputlevel, float echoSend = 1.0f,
                          bool noiseEnabled = false, float noiseAmount = 0.12f, float tapeCondition = 0.08f,
                          float wowFlutterDepth = 1.0f, float wowRate = 1.0f, float flutterRate = 1.0f,
                          int stereoMode = 0, bool tempoSyncEnabled = false, float head1SyncDelayMs = 0.0f,
                          float head2SyncDelayMs = 0.0f, float head3SyncDelayMs = 0.0f, bool binauralEnabled = false,
                          float binauralAzimuth = 0.0f, float binauralElevation = 0.0f, float binauralDistance = 1.0f,
                          float binauralWetMix = 100.0f, float binauralCrossover = 150.0f, int binauralQuality = 2,
                          float tapeDrive = 0.5f, float tapeSaturation = 0.5f, float tapeBias = 0.5f,
                          int headRatioPreset = 0, int reverbRoutingMode = 0, float presetAsperity = -1.0f,
                          float presetDropout = -1.0f, float presetScrapeScale = 1.0f, float presetLpfScale = 1.0f,
                          bool feedbackCompensationEnabled = true, int spatialRoutingMode = 0,
                          float echoLevelGain = 1.0f, float reverbLevelGain = 1.0f, float reverbDuckAmount = 0.0f,
                          const ReverbMacroParams& reverbMacros = {}, const TapSpatialParams& tapSpatial = {},
                          float feedbackUserIntensity = -1.0f);
    void ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize);

    float getEchoBranchMeterLevel() const noexcept { return echoBranchMeterLinear.load(std::memory_order_relaxed); }
    float getReverbBranchMeterLevel() const noexcept { return reverbBranchMeterLinear.load(std::memory_order_relaxed); }
    // Per-tap envelope levels (peak-envelope of delay head tap outputs, post head-level, pre spatial mix).
    // tapIndex 0..2; out-of-range returns 0. Same attack/release character as echoBranchMeterLinear.
    float getTapEnvelopeLevel(int tapIndex) const noexcept
    {
        if (tapIndex < 0 || tapIndex >= 3)
            return 0.0f;
        return tapEnvelopeMeterLinear[static_cast<size_t>(tapIndex)].load(std::memory_order_relaxed);
    }
    // Self-oscillation heat (tape saturator drive) for the PANIC light. See TapeDelayProcessor::getSelfOscHeat.
    float getSelfOscHeat() const noexcept { return TapeDelay ? TapeDelay->getSelfOscHeat() : 0.0f; }

    // Kill/Clear buffer - triggers smooth fade-out then clears delay buffers
    void triggerKill() { TapeDelay->triggerKill(); }

    // Cancel ongoing kill fade (when power turned back on)
    void cancelKill() { TapeDelay->cancelKill(); }

    // Auto-panic flush (called from the processor's auto-panic watchdog; see
    // SpatialEchoAudioProcessor::runAutoPanicWatchdog). Audio-thread safe.
    // fadeOut=true - finite runaway: the tape loop fades over the normal kill
    //   time and clears. Reverb uses a hard safety clear, which may truncate
    //   its current tail.
    // fadeOut=false - non-finite state: hard immediate clear. A fade is
    //   meaningless on NaN (NaN times any gain is still NaN), and every
    //   poisoned recursive filter must be zeroed now or it re-poisons the
    //   freshly cleared buffers on the next block.
    void autoPanicFlush(bool fadeOut);
    // Peak normalization for the watchdog's runaway gate. This removes only
    // post-loop presentation gain (wet mix / branch trim / Echo Level) so the
    // detector judges loop stability, not user-facing loudness trim.
    float getAutoPanicRunawayMonitorNormalization() const noexcept { return autoPanicRunawayMonitorNormalization; }

    void setTapeRecordMakeupGainDb(float makeupGainDb);

    // Voice 0 of the spatial field; tests and diagnostics inspect it directly.
    BinauralProcessor* getBinauralProcessor()
    {
        SpatialField* field = getSpatialField();
        return field ? field->getVoiceProcessor(0) : nullptr;
    }
    const BinauralProcessor* getBinauralProcessor() const
    {
        const SpatialField* field = getSpatialField();
        return field ? field->getVoiceProcessor(0) : nullptr;
    }

    SpatialField* getSpatialField() { return spatialField.get(); }
    const SpatialField* getSpatialField() const { return spatialField.get(); }

    TapeDelayProcessor* getTapeDelay() { return TapeDelay.get(); }
    const TapeDelayProcessor* getTapeDelay() const { return TapeDelay.get(); }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Per-stage processing time, read through EchoesModelTestAccess.
    struct StageTimingSnapshot
    {
        uint64_t blocks = 0;
        uint64_t totalUs = 0;
        uint64_t preampUs = 0;
        uint64_t reverbUs = 0;
        uint64_t delayUs = 0;
        uint64_t toneUs = 0;
        uint64_t mixUs = 0;
        uint64_t noiseUs = 0;
        uint64_t binauralUs = 0;
        uint64_t maxBlockUs = 0;
        uint64_t maxPreampUs = 0;
        uint64_t maxReverbUs = 0;
        uint64_t maxDelayUs = 0;
        uint64_t maxToneUs = 0;
        uint64_t maxMixUs = 0;
        uint64_t maxNoiseUs = 0;
        uint64_t maxBinauralUs = 0;
    };
#endif

private:
    friend struct EchoesModelTestAccess;

    // DSP objects
    std::unique_ptr<PreampProcessor> Preamp = std::make_unique<PreampProcessor>();
    std::array<std::unique_ptr<ToneStackProcessor>, 3> ToneStacks;
    std::unique_ptr<TapeDelayProcessor> TapeDelay = std::make_unique<TapeDelayProcessor>();
    std::unique_ptr<ReverbProcessor> Reverb = std::make_unique<ReverbProcessor>();
    std::unique_ptr<SpatialField> spatialField = std::make_unique<SpatialField>();

    // buffers for parallel processing
    std::vector<std::vector<float>> reverbbuffer;           // Preamp'd signal for reverb
    std::vector<std::vector<float>> reverbRecoveredBuffer;  // Post-recovery reverb (for mix / routing)
    std::vector<std::vector<float>> drybuffer;              // Unprocessed input for transparent dry path
    std::vector<std::vector<float>> echoPostToneBuffer;     // Echo signal after tone stack, before output gain
    std::vector<std::vector<float>> processingBuffer;       // Internal processing buffer for chunking/mismatch cases
    std::array<std::vector<std::vector<float>>, 3> headPostToneBuffers;  // Canonical treated tap buses
    std::vector<std::vector<float>> spatializedHeadsSum;                 // Sum of spatialized taps (Independent mode)

    // Plugin info
    float SampleRate = 0.0f;
    int NumChannels = 2;
    int BlockSize = 512;

    bool DelayEnabled = false;
    bool ReverbEnabled = false;
    float ReverbAmount = 0.0f;      // Fixed internal trim for reverb output gain
    int ReverbRoutingMode = 0;      // 0 = Parallel (classic), 1 = Echo->Reverb, 2 = Reverb->Echo
    float EchoSend = 1.0f;          // Cached for routing decisions (1.0 = Echo mode, 0.0 = Dub)
    float DelayAmount = 0.0f;       // Echo output level (applied after tone stack)
    float WetMix = 0.0f;            // Wet level (0=dry/transparent, 1=full wet added to dry)
    float EchoLevelGain = 1.0f;     // Independent echo branch trim
    float ReverbLevelGain = 1.0f;   // Independent reverb branch trim
    float ReverbDuckAmount = 0.0f;  // Dry-input ducking applied only to audible reverb wet branch
    float reverbDuckEnvelope = 0.0f;
    bool needsInitialParameterSnap = true;
    static constexpr double kMixSmoothingTimeSeconds = 0.01;
    static constexpr double kBalanceSmoothingTimeSeconds = 0.01;
    static constexpr double kBranchLevelSmoothingTimeSeconds = 0.01;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetMixSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> branchBalanceSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> echoLevelSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbLevelSmoother;
    std::vector<float> dryGainRamp;
    std::vector<float> wetGainRamp;
    std::vector<float> echoGainRamp;
    std::vector<float> reverbGainRamp;
    std::vector<float> echoLevelRamp;
    std::vector<float> reverbLevelRamp;
    float autoPanicRunawayMonitorNormalization = 1.0f;
    std::atomic<float> echoBranchMeterLinear{0.0f};
    std::atomic<float> reverbBranchMeterLinear{0.0f};
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Test level probes: ProcessBuffer writes them while enabled, and
    // EchoesModelTestAccess reads and controls them. Reverb slots are indexed
    // by routing mode (0-2).
    std::atomic<float> debugEchoPathPreTrimPeak{0.0f};
    std::atomic<float> debugEchoPathPreTrimRms{0.0f};
    std::atomic<bool> debugEchoPathPreTrimProbeEnabled{false};
    std::atomic<bool> debugReverbInputProbeEnabled{false};
    std::array<std::atomic<float>, 3> debugReverbInputPeak{std::atomic<float>(0.0f), std::atomic<float>(0.0f),
                                                           std::atomic<float>(0.0f)};
    std::array<std::atomic<float>, 3> debugReverbInputRms{std::atomic<float>(0.0f), std::atomic<float>(0.0f),
                                                          std::atomic<float>(0.0f)};
    std::atomic<bool> debugReverbReturnProbeEnabled{false};
    std::array<std::atomic<float>, 3> debugReverbReturnPeak{std::atomic<float>(0.0f), std::atomic<float>(0.0f),
                                                            std::atomic<float>(0.0f)};
    std::array<std::atomic<float>, 3> debugReverbReturnRms{std::atomic<float>(0.0f), std::atomic<float>(0.0f),
                                                           std::atomic<float>(0.0f)};

    void resetTestProbes() noexcept;
    // Peak and RMS over the first numChannels channels of numSamples samples.
    static void storeProbeLevels(const std::vector<std::vector<float>>& buffers, int numChannels, int numSamples,
                                 std::atomic<float>& peak, std::atomic<float>& rms) noexcept;
#endif
    float echoMeterEnvelope = 0.0f;
    float reverbMeterEnvelope = 0.0f;

    // Per-tap envelope meters: track peak-envelope of each delay head's tap output
    // (post head-level, pre spatial mix). Updated in audio thread via atomic stores.
    std::array<std::atomic<float>, 3> tapEnvelopeMeterLinear = {std::atomic<float>(0.0f), std::atomic<float>(0.0f),
                                                                std::atomic<float>(0.0f)};
    std::array<float, 3> tapEnvelopeMeterEnvelope = {0.0f, 0.0f, 0.0f};

    // Binaural parameters
    bool BinauralEnabled = false;
    int SpatialRoutingMode = 0;  // 0 = After Effects (default), 1 = Before Effects

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Auto-panic test fault: added to every post-mix sample when non-zero.
    float debugWetBusFault = 0.0f;
#endif

    // Per-tap spatial parameters (Independent mode routing)
    // 0 = Linked (all taps follow global binaural position, HEAD parity)
    // 1 = Independent (each tap gets separate binaural voice with per-tap 3D position)
    int TapSpatialMode = 0;
    struct TapSpatialParams tapSpatialParams;
    bool independentSpatialWasActive = false;
    bool tapVoicesNeedPositionSnap = true;  // Snap voice positions on first Independent block, then glide

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    struct StageTimingAccumulator
    {
        std::atomic<uint64_t> blocks{0};
        std::atomic<uint64_t> totalUs{0};
        std::atomic<uint64_t> preampUs{0};
        std::atomic<uint64_t> reverbUs{0};
        std::atomic<uint64_t> delayUs{0};
        std::atomic<uint64_t> toneUs{0};
        std::atomic<uint64_t> mixUs{0};
        std::atomic<uint64_t> noiseUs{0};
        std::atomic<uint64_t> binauralUs{0};
        std::atomic<uint64_t> maxBlockUs{0};
        std::atomic<uint64_t> maxPreampUs{0};
        std::atomic<uint64_t> maxReverbUs{0};
        std::atomic<uint64_t> maxDelayUs{0};
        std::atomic<uint64_t> maxToneUs{0};
        std::atomic<uint64_t> maxMixUs{0};
        std::atomic<uint64_t> maxNoiseUs{0};
        std::atomic<uint64_t> maxBinauralUs{0};

        void reset()
        {
            blocks.store(0, std::memory_order_relaxed);
            totalUs.store(0, std::memory_order_relaxed);
            preampUs.store(0, std::memory_order_relaxed);
            reverbUs.store(0, std::memory_order_relaxed);
            delayUs.store(0, std::memory_order_relaxed);
            toneUs.store(0, std::memory_order_relaxed);
            mixUs.store(0, std::memory_order_relaxed);
            noiseUs.store(0, std::memory_order_relaxed);
            binauralUs.store(0, std::memory_order_relaxed);
            maxBlockUs.store(0, std::memory_order_relaxed);
            maxPreampUs.store(0, std::memory_order_relaxed);
            maxReverbUs.store(0, std::memory_order_relaxed);
            maxDelayUs.store(0, std::memory_order_relaxed);
            maxToneUs.store(0, std::memory_order_relaxed);
            maxMixUs.store(0, std::memory_order_relaxed);
            maxNoiseUs.store(0, std::memory_order_relaxed);
            maxBinauralUs.store(0, std::memory_order_relaxed);
        }
    };

    std::atomic<bool> stageTimingEnabled{false};
    StageTimingAccumulator stageTiming;
#endif
};
