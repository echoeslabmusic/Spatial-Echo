// EchoesModel: Top-level signal routing and parameter coordination.
// Realtime-safety: No allocation/locks in process path; smoothers for parameter updates

#include "EchoesModel.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

// NOTE [DESIGN CONTEXT]:
// Comments tagged with "Design" describe intentional routing and tuning choices.
// Update rationale if you change them.

EchoesModel::EchoesModel()
{
}

EchoesModel::~EchoesModel()
{
}

namespace
{
// Wet-branch calibration:
// Presentation makeup is applied only to the audible echo branch after
// tape tone and any per-head Independent rendering. The first-party record
// stage is small-signal-unity, so this value is the measured branch
// correction for it; a larger loss compensation, sized for a lossy record
// stage, would overdrive ordinary first repeats. Feedback and Echo-to-Reverb send
// levels remain independent of this presentation boundary.
constexpr float kDelayPresentationMakeup = 1.925f;
constexpr float kCanonicalReverbProcessorDrive = 1.00f;
constexpr float kReverbSendPad = 0.50f;
constexpr float kReverbDuckThreshold = 0.02f;
constexpr float kReverbDuckMaxAttenuationDb = -24.0f;
constexpr float kReverbDuckAttackSeconds = 0.003f;
constexpr float kReverbDuckReleaseSeconds = 0.180f;

inline float onePoleCoeff(float timeSeconds, float sampleRate)
{
    return (sampleRate > 0.0f && timeSeconds > 0.0f) ? std::exp(-1.0f / (timeSeconds * sampleRate)) : 0.0f;
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
using StageClock = std::chrono::steady_clock;

inline uint64_t toMicros(StageClock::duration duration)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

inline void updateMaxAtomic(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}
#endif
}

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
void EchoesModel::resetTestProbes() noexcept
{
    debugEchoPathPreTrimPeak.store(0.0f, std::memory_order_relaxed);
    debugEchoPathPreTrimRms.store(0.0f, std::memory_order_relaxed);
    for (size_t route = 0; route < 3; ++route)
    {
        debugReverbInputPeak[route].store(0.0f, std::memory_order_relaxed);
        debugReverbInputRms[route].store(0.0f, std::memory_order_relaxed);
        debugReverbReturnPeak[route].store(0.0f, std::memory_order_relaxed);
        debugReverbReturnRms[route].store(0.0f, std::memory_order_relaxed);
    }
}

void EchoesModel::storeProbeLevels(const std::vector<std::vector<float>>& buffers, int numChannels, int numSamples,
                                   std::atomic<float>& peak, std::atomic<float>& rms) noexcept
{
    float peakValue = 0.0f;
    double energy = 0.0;
    for (size_t ch = 0; ch < static_cast<size_t>(std::max(0, numChannels)); ++ch)
    {
        for (size_t s = 0; s < static_cast<size_t>(std::max(0, numSamples)); ++s)
        {
            const float value = buffers[ch][s];
            peakValue = std::max(peakValue, std::abs(value));
            energy += static_cast<double>(value) * static_cast<double>(value);
        }
    }
    const int valueCount = numChannels * numSamples;
    peak.store(peakValue, std::memory_order_relaxed);
    rms.store(valueCount > 0 ? static_cast<float>(std::sqrt(energy / valueCount)) : 0.0f, std::memory_order_relaxed);
}
#endif

void EchoesModel::Reset(float sampleRate, int numChannels, int maxBlockSize)
{
    // Save channel count and sample rate
    NumChannels = numChannels;
    SampleRate = sampleRate;
    BlockSize = std::max(1, maxBlockSize);

    // Initialise DSP objects
    Preamp.reset(new PreampProcessor());
    for (auto& toneStack : ToneStacks)
        toneStack = std::make_unique<ToneStackProcessor>();
    TapeDelay.reset(new TapeDelayProcessor());
    Reverb.reset(new ReverbProcessor());
    spatialField.reset(new SpatialField());

    // Reset DSP objects
    Preamp->reset(sampleRate, NumChannels, maxBlockSize);
    for (auto& toneStack : ToneStacks)
        toneStack->Reset(sampleRate, NumChannels);
    TapeDelay->Reset(sampleRate, NumChannels, BlockSize);
    Reverb->Reset(sampleRate, NumChannels, BlockSize);
    spatialField->prepare(sampleRate, maxBlockSize, NumChannels);
    tapVoicesNeedPositionSnap = true;
    independentSpatialWasActive = false;

// Enable binaural diagnostics in debug builds (voice 0)
#if JUCE_DEBUG
    if (auto* binaural = spatialField->getVoiceProcessor(0))
        binaural->setDiagnosticsEnabled(true);
#endif

    // Reverb recovery EQ is handled inside ReverbProcessor to avoid double-filtering.

    // Preallocate internal buffers to avoid RT allocations in ProcessBuffer.
    auto initBuffer = [&](std::vector<std::vector<float>>& dst)
    {
        dst.resize((size_t)NumChannels);
        for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
            dst[ch].assign((size_t)BlockSize, 0.0f);
    };

    initBuffer(reverbbuffer);
    initBuffer(reverbRecoveredBuffer);
    initBuffer(drybuffer);
    initBuffer(echoPostToneBuffer);
    initBuffer(processingBuffer);
    for (auto& headBuffer : headPostToneBuffers)
        initBuffer(headBuffer);
    initBuffer(spatializedHeadsSum);
    dryGainRamp.assign((size_t)BlockSize, 1.0f);
    wetGainRamp.assign((size_t)BlockSize, 0.0f);
    echoGainRamp.assign((size_t)BlockSize, 1.0f);
    reverbGainRamp.assign((size_t)BlockSize, 0.0f);
    echoLevelRamp.assign((size_t)BlockSize, 1.0f);
    reverbLevelRamp.assign((size_t)BlockSize, 1.0f);

    if (sampleRate > 0.0)
    {
        wetMixSmoother.reset(sampleRate, kMixSmoothingTimeSeconds);
        branchBalanceSmoother.reset(sampleRate, kBalanceSmoothingTimeSeconds);
        echoLevelSmoother.reset(sampleRate, kBranchLevelSmoothingTimeSeconds);
        reverbLevelSmoother.reset(sampleRate, kBranchLevelSmoothingTimeSeconds);
    }
    const float modePinnedTarget = (DelayEnabled && ReverbEnabled) ? 0.5f : (ReverbEnabled ? 1.0f : 0.0f);
    wetMixSmoother.setCurrentAndTargetValue(WetMix);
    branchBalanceSmoother.setCurrentAndTargetValue(modePinnedTarget);
    echoLevelSmoother.setCurrentAndTargetValue(EchoLevelGain);
    reverbLevelSmoother.setCurrentAndTargetValue(ReverbLevelGain);
    echoMeterEnvelope = 0.0f;
    reverbMeterEnvelope = 0.0f;
    reverbDuckEnvelope = 0.0f;
    needsInitialParameterSnap = true;
    echoBranchMeterLinear.store(0.0f, std::memory_order_relaxed);
    reverbBranchMeterLinear.store(0.0f, std::memory_order_relaxed);
    for (size_t h = 0; h < 3; ++h)
    {
        tapEnvelopeMeterEnvelope[h] = 0.0f;
        tapEnvelopeMeterLinear[h].store(0.0f, std::memory_order_relaxed);
    }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    stageTiming.reset();
    resetTestProbes();
#endif
}

void EchoesModel::UpdateParameters(float low, float high, float intensity, float delaytime, int playheadenabled[],
                                   int delayenabled, float mix, int reverbenabled, float inputlevel, float echoSend,
                                   bool noiseEnabled, float noiseAmount, float tapeCondition, float wowFlutterDepth,
                                   float wowRate, float flutterRate, int stereoMode, bool tempoSyncEnabled,
                                   float head1SyncDelayMs, float head2SyncDelayMs, float head3SyncDelayMs,
                                   bool binauralEnabled, float binauralAzimuth, float binauralElevation,
                                   float binauralDistance, float binauralWetMix, float binauralCrossover,
                                   int binauralQuality, float tapeDrive, float tapeSaturation, float tapeBias,
                                   int headRatioPreset, int reverbRoutingMode, float presetAsperity,
                                   float presetDropout, float presetScrapeScale, float presetLpfScale,
                                   bool feedbackCompensationEnabled, int spatialRoutingMode, float echoLevelGain,
                                   float reverbLevelGain, float reverbDuckAmount, const ReverbMacroParams& reverbMacros,
                                   const TapSpatialParams& tapSpatial, float feedbackUserIntensity)
{
    // Per-tap spatial positioning (TapSpatialParams)
    // positionMode: 0=Linked (follow global binaural), 1=Independent (per-tap 3D positions)
    // Linked mode: all taps use voice 0, follow global binaural position (HEAD parity)
    // Independent mode: each tap (H1/H2/H3) routed to separate binaural voices with per-tap spatial positions
    TapSpatialMode = std::clamp(tapSpatial.positionMode, 0, 1);
    tapSpatialParams = tapSpatial;

    // Enable per-head capture in TapeDelay to support per-tap meter levels.
    // Used by Independent mode spatial routing AND by per-tap envelope metering for UI feedback.
    if (TapeDelay)
    {
        TapeDelay->enablePerHeadCapture(true);
    }

    SpatialRoutingMode = std::clamp(spatialRoutingMode, 0, 1);
    DelayEnabled = delayenabled != 0;
    ReverbEnabled = reverbenabled != 0;
    const float modePinnedTarget = (DelayEnabled && ReverbEnabled) ? 0.5f : (ReverbEnabled ? 1.0f : 0.0f);
    branchBalanceSmoother.setTargetValue(modePinnedTarget);
    ReverbRoutingMode = std::clamp(reverbRoutingMode, 0, 2);
    ReverbAmount = (reverbenabled != 0) ? kCanonicalReverbProcessorDrive : 0.0f;
    EchoSend = echoSend;
    // Keep presentation makeup stable and let TapeDelay handle click-safe enable/disable fades.
    DelayAmount = kDelayPresentationMakeup;
    WetMix = juce::jlimit(0.0f, 1.0f, mix);
    EchoLevelGain = juce::jlimit(0.0f, 4.0f, echoLevelGain);
    ReverbLevelGain = juce::jlimit(0.0f, 4.0f, reverbLevelGain);
    ReverbDuckAmount = juce::jlimit(0.0f, 1.0f, reverbDuckAmount);
    wetMixSmoother.setTargetValue(WetMix);
    echoLevelSmoother.setTargetValue(EchoLevelGain);
    reverbLevelSmoother.setTargetValue(ReverbLevelGain);

    // Update DSP object parameters
    // Design: InputLevel directly controls preamp saturation
    Preamp->updateParameters(inputlevel);
    for (auto& toneStack : ToneStacks)
        toneStack->UpdateParameters(low, high, 1.0f);  // InputLevel handled by Preamp

    TapeDelay->setTapeCharacter(tapeDrive, tapeSaturation, tapeBias);

    TapeDelay->UpdateParameters(intensity, delaytime, playheadenabled, delayenabled, echoSend, noiseEnabled,
                                noiseAmount, tapeCondition, wowFlutterDepth, wowRate, flutterRate, tempoSyncEnabled,
                                head1SyncDelayMs, head2SyncDelayMs, head3SyncDelayMs, stereoMode, headRatioPreset,
                                feedbackCompensationEnabled, presetAsperity, presetDropout, presetScrapeScale,
                                presetLpfScale, feedbackUserIntensity);
    Reverb->UpdateParameters(reverbenabled, ReverbAmount, reverbMacros, noiseEnabled, noiseAmount);

    // Store and update renderer configuration for every spatial voice.
    BinauralEnabled = binauralEnabled;
    if (spatialField)
    {
        spatialField->setEnabled(binauralEnabled);
        // In Independent mode voice 0 renders tap 1 at its per-tap position
        // (set each block in ProcessBuffer). Pushing the global position here
        // too would retarget the voice's position smoothing every block and
        // leave it rendering mid-glide between the two targets.
        if (TapSpatialMode != 1)
        {
            spatialField->setVoiceAzimuth(0, binauralAzimuth);
            spatialField->setVoiceElevation(0, binauralElevation);
            spatialField->setVoiceDistance(0, binauralDistance);
        }
        spatialField->setWetMix(binauralWetMix);
        spatialField->setCrossoverFrequency(binauralCrossover);
        const auto quality = static_cast<BinauralProcessor::HRTFQuality>(binauralQuality);
        spatialField->setHRTFQuality(quality);
    }

    if (needsInitialParameterSnap)
    {
        wetMixSmoother.setCurrentAndTargetValue(WetMix);
        branchBalanceSmoother.setCurrentAndTargetValue(modePinnedTarget);
        echoLevelSmoother.setCurrentAndTargetValue(EchoLevelGain);
        reverbLevelSmoother.setCurrentAndTargetValue(ReverbLevelGain);
        if (spatialField)
            spatialField->reset();
        needsInitialParameterSnap = false;
    }
}

void EchoesModel::ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize)
{
    if (blockSize <= 0 || NumChannels <= 0)
        return;

    const int inputChannels = static_cast<int>(buffer.size());
    if (inputChannels <= 0)
        return;

    // Clamp to available samples to avoid out-of-bounds if host buffer is undersized.
    int availableSamples = blockSize;
    for (size_t ch = 0; ch < static_cast<size_t>(inputChannels); ++ch)
        availableSamples = std::min(availableSamples, static_cast<int>(buffer[ch].size()));
    if (availableSamples <= 0)
        return;

    const int targetChannels = juce::jmax(1, NumChannels);
    const int maxChunk = std::max(1, BlockSize);
    const bool canProcessInPlace = (availableSamples <= maxChunk) && (inputChannels >= targetChannels);

    auto copyBuffer =
        [&](const std::vector<std::vector<float>>& src, std::vector<std::vector<float>>& dst, int numSamples)
    {
        const size_t bytes = sizeof(float) * (size_t)numSamples;
        for (size_t ch = 0; ch < static_cast<size_t>(targetChannels); ++ch)
            std::memcpy(dst[ch].data(), src[ch].data(), bytes);
    };

    auto processBlockInternal = [&](std::vector<std::vector<float>>& workBuffer, int numSamples)
    {
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
        const bool captureTiming = stageTimingEnabled.load(std::memory_order_relaxed);
        const auto blockStart = captureTiming ? StageClock::now() : StageClock::time_point{};

#define SPATIAL_ECHO_MEASURE_STAGE(totalField, maxField, code)                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!captureTiming)                                                                                            \
        {                                                                                                              \
            code;                                                                                                      \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            const auto _stageStart = StageClock::now();                                                                \
            code;                                                                                                      \
            const auto _elapsedUs = toMicros(StageClock::now() - _stageStart);                                         \
            stageTiming.totalField.fetch_add(_elapsedUs, std::memory_order_relaxed);                                   \
            updateMaxAtomic(stageTiming.maxField, _elapsedUs);                                                         \
        }                                                                                                              \
    } while (0)
#else
#define SPATIAL_ECHO_MEASURE_STAGE(totalField, maxField, code)                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        code;                                                                                                          \
    } while (0)
#endif

        // Step 1: Capture transparent dry path before any coloration.
        copyBuffer(workBuffer, drybuffer, numSamples);

        // Step 2: Preamp (input stage) for wet/effect processing.
        SPATIAL_ECHO_MEASURE_STAGE(preampUs, maxPreampUs, { Preamp->processBuffer(workBuffer, numSamples); });

        const bool doIndependentSpatialRouting = (TapSpatialMode == 1 && BinauralEnabled && spatialField);
        if (doIndependentSpatialRouting != independentSpatialWasActive)
        {
            // Linked owns voice 0; Independent owns voices 1..3. Clear only
            // the renderer that became dormant. This keeps frozen tails out
            // of either re-entry direction without truncating an active voice.
            if (doIndependentSpatialRouting)
            {
                spatialField->resetVoice(SpatialField::kGlobalVoiceIndex);
            }
            else
            {
                for (int headIdx = 0; headIdx < SpatialField::kTapVoiceCount; ++headIdx)
                    spatialField->resetVoice(SpatialField::kTapVoiceOffset + headIdx);
                tapVoicesNeedPositionSnap = true;
            }
            independentSpatialWasActive = doIndependentSpatialRouting;
        }

        // Helper: run spatial field processing on workBuffer (unconditional - internal smoother handles enable/disable)
        auto applyBinauralToWorkBuffer = [&]()
        {
            const int spatialChannels = (NumChannels >= 2) ? 2 : 1;
            float* channelPointers[2];
            channelPointers[0] = workBuffer[0].data();
            if (spatialChannels > 1)
                channelPointers[1] = workBuffer[1].data();
            juce::AudioBuffer<float> audioBuffer(channelPointers, spatialChannels, numSamples);
            if (spatialField)
                spatialField->processBlock(audioBuffer);
        };

        // Step 2b: Spatial routing - Before Effects mode (mode 1)
        // Applies binaural BEFORE reverb/delay so echoes and reverb tail inherit the spatial position.
        if (SpatialRoutingMode == 1 && !doIndependentSpatialRouting)
        {
            SPATIAL_ECHO_MEASURE_STAGE(binauralUs, maxBinauralUs, { applyBinauralToWorkBuffer(); });
        }

        // Generate the tank's noise send from this chunk's preamp input before
        // either serial route consumes it. Reading the delay's previous callback
        // here repeated/skipped noise samples when host callback sizes changed.
        TapeDelay->prepareReverbNoiseBuffer(workBuffer, numSamples);

        // Step 3: Reverb routing (parallel by default; optional serial modes)
        int routingMode = ReverbRoutingMode;
        if (!ReverbEnabled || ReverbAmount <= 0.0f || !TapeDelay->isDelayEnabled())
            routingMode = 0;  // Reverb-only or Echo-only ignores routing extensions

        // Test level probes live outside the stage macro below: MSVC rejects
        // preprocessor directives inside macro arguments.
        auto probeReverbInput = [&]()
        {
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
            if (debugReverbInputProbeEnabled.load(std::memory_order_relaxed))
            {
                const auto routeIndex = static_cast<size_t>(std::clamp(routingMode, 0, 2));
                storeProbeLevels(reverbbuffer, NumChannels, numSamples, debugReverbInputPeak[routeIndex],
                                 debugReverbInputRms[routeIndex]);
            }
#endif
        };

        auto probeReverbReturn = [&]()
        {
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
            if (debugReverbReturnProbeEnabled.load(std::memory_order_relaxed))
            {
                const auto routeIndex = static_cast<size_t>(std::clamp(routingMode, 0, 2));
                storeProbeLevels(reverbRecoveredBuffer, NumChannels, numSamples, debugReverbReturnPeak[routeIndex],
                                 debugReverbReturnRms[routeIndex]);
            }
#endif
        };

        auto processReverbBuffer = [&]()
        {
            SPATIAL_ECHO_MEASURE_STAGE(reverbUs, maxReverbUs, {
                if (ReverbEnabled)
                {
                    // The generator owns enable/amount smoothing and exact
                    // silence. A second parameter gate here would cut its fade.
                    for (int ch = 0; ch < NumChannels; ++ch)
                    {
                        const auto& reverbNoiseBuffer = TapeDelay->getReverbNoiseBufferForChannel(ch);
                        const int reverbNoiseCount = static_cast<int>(reverbNoiseBuffer.size());
                        const int copyCount = std::min(numSamples, reverbNoiseCount);
                        for (int s = 0; s < copyCount; ++s)
                            reverbbuffer[static_cast<size_t>(ch)][static_cast<size_t>(s)] +=
                                reverbNoiseBuffer[(size_t)s];
                    }
                }

                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                    for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                        reverbbuffer[ch][s] *= kReverbSendPad;

                probeReverbInput();

                Reverb->ProcessBuffer(reverbbuffer, numSamples);

                // No makeup gain. The reverb processor is unity in-band at
                // presentation amount 1.0 (ReverbInsertionGain checks this), so
                // a per-block makeup factor here would only add level error and
                // step the reverb bus whenever the delay toggles.
                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                {
                    for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                    {
                        reverbRecoveredBuffer[ch][s] = reverbbuffer[ch][s];
                    }
                }

                probeReverbReturn();
            });
        };

        auto renderReverbFrom = [&](const std::vector<std::vector<float>>& src)
        {
            copyBuffer(src, reverbbuffer, numSamples);
            processReverbBuffer();
        };

        if (routingMode == 2)
        {
            if (ReverbEnabled && ReverbAmount > 0.0f)
            {
                renderReverbFrom(workBuffer);

                // True serial: delay processes reverb output (symmetric with Echo->Reverb)
                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                    for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                        workBuffer[ch][s] = reverbRecoveredBuffer[ch][s];
            }
        }
        else if (routingMode == 0)
        {
            if (ReverbEnabled && ReverbAmount > 0.0f)
                renderReverbFrom(workBuffer);
        }

        // Step 4: Tape delay processing.
        // Always process so DelayEnabled/EchoSend ramps in TapeDelay can fade click-free.
        SPATIAL_ECHO_MEASURE_STAGE(delayUs, maxDelayUs, { TapeDelay->ProcessBuffer(workBuffer, numSamples); });

        // Step 4b: Update per-tap envelope meters.
        // Measure peak-envelope of each delay head's tap output (post head-level, pre spatial mix).
        // Fast UI peak-envelope ballistics: 180 ms release. This is
        // intentionally neither VU nor PPM behavior.
        {
            const float releaseTimeSec = 0.18f;
            const float tapRelease = (SampleRate > 0.0f && numSamples > 0)
                                         ? std::exp(-static_cast<float>(numSamples) / (releaseTimeSec * SampleRate))
                                         : 0.0f;

            for (int headIdx = 0; headIdx < 3; ++headIdx)
            {
                bool hasCapture = false;
                float headPeakLinear = 0.0f;
                for (int ch = 0; ch < NumChannels; ++ch)
                {
                    const float* headCapture = TapeDelay->getHeadOutput(headIdx, ch);
                    if (!headCapture)
                        continue;

                    hasCapture = true;
                    for (int s = 0; s < numSamples; ++s)
                        headPeakLinear = std::max(headPeakLinear, std::abs(headCapture[s]));
                }

                tapEnvelopeMeterEnvelope[static_cast<size_t>(headIdx)] =
                    hasCapture ? juce::jmax(headPeakLinear,
                                            tapEnvelopeMeterEnvelope[static_cast<size_t>(headIdx)] * tapRelease)
                               : tapEnvelopeMeterEnvelope[static_cast<size_t>(headIdx)] * tapRelease;

                // Publish to UI (atomic, relaxed ordering)
                tapEnvelopeMeterLinear[static_cast<size_t>(headIdx)].store(
                    tapEnvelopeMeterEnvelope[static_cast<size_t>(headIdx)], std::memory_order_relaxed);
            }
        }

        // Step 5: Apply the echo tone stack to each canonical head bus. The
        // three filters use identical parameters and independent state, so
        // summing these buses reconstructs the Linked echo while preserving
        // the exact post-tone signals needed by Independent mode.
        //
        // ROUTING INVARIANT - do not break this. The reconstruction is exact
        // only because the tone stack is strictly linear AND all three
        // instances receive an IDENTICAL call sequence. Measured error is
        // -127.8 dB (float32 rounding floor) with block size varied and both
        // tone controls swept every block. Give one instance a single extra
        // ProcessBuffer call and the error jumps to -57 dB, permanently: the
        // early-return guards in ToneStackProcessor::ProcessBuffer skip the
        // gain-smoother advance, so once the three smoothers desync they never
        // resync. So: always loop all three, always with the same numSamples,
        // never conditionally skip one. ToneStackSumLinearity gates this.
        SPATIAL_ECHO_MEASURE_STAGE(toneUs, maxToneUs, {
            for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                std::fill_n(workBuffer[ch].begin(), (size_t)numSamples, 0.0f);

            for (int headIdx = 0; headIdx < 3; ++headIdx)
            {
                for (int ch = 0; ch < NumChannels; ++ch)
                {
                    auto& headBuffer = headPostToneBuffers[(size_t)headIdx][static_cast<size_t>(ch)];
                    if (const float* headCapture = TapeDelay->getHeadOutput(headIdx, ch))
                        std::copy_n(headCapture, numSamples, headBuffer.begin());
                    else
                        std::fill_n(headBuffer.begin(), (size_t)numSamples, 0.0f);
                }

                ToneStacks[(size_t)headIdx]->ProcessBuffer(headPostToneBuffers[(size_t)headIdx], numSamples);

                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                    for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                        workBuffer[ch][s] += headPostToneBuffers[(size_t)headIdx][ch][s];
            }
        });

        // Capture post-tone echo BEFORE output level for routing sends
        copyBuffer(workBuffer, echoPostToneBuffer, numSamples);

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
        if (debugEchoPathPreTrimProbeEnabled.load(std::memory_order_relaxed))
            storeProbeLevels(echoPostToneBuffer, NumChannels, numSamples, debugEchoPathPreTrimPeak,
                             debugEchoPathPreTrimRms);
#endif

        // Step 5c: Independent mode spatial routing
        // In Independent mode, each tap (H1/H2/H3) is rendered as a separate binaural voice
        // with per-tap spatial positions, and the three spatialized signals are summed to
        // replace the echo branch output. Dry and reverb branches remain unspatialized.
        // Linked mode: all taps follow the global binaural position (HEAD parity).
        if (doIndependentSpatialRouting)
        {
            // Pre-allocated in Reset(); no allocation on the audio thread.
            for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                std::fill_n(spatializedHeadsSum[ch].begin(), (size_t)numSamples, 0.0f);

            // In Independent mode, three dedicated voices are used for the
            // tap-specific positions. Global voice 0 remains dormant; heads
            // 0..2 route to voices kTapVoiceOffset..kTapVoiceOffset + 2.

            const float tapAz[3] = {tapSpatialParams.h1Azimuth, tapSpatialParams.h2Azimuth, tapSpatialParams.h3Azimuth};
            const float tapEl[3] = {tapSpatialParams.h1Elevation, tapSpatialParams.h2Elevation,
                                    tapSpatialParams.h3Elevation};
            const float tapDist[3] = {tapSpatialParams.h1Distance, tapSpatialParams.h2Distance,
                                      tapSpatialParams.h3Distance};

            for (int headIdx = 0; headIdx < 3; ++headIdx)
            {
                // Each head renders through ITS OWN voice at its own position.
                // (Calling spatialField->processBlock here would run every enabled
                // voice - so we process one voice at a time per head.)
                BinauralProcessor* voiceProc = spatialField->getVoiceProcessor(SpatialField::kTapVoiceOffset + headIdx);
                if (!voiceProc)
                    continue;

                // First block after Reset / re-entering Independent mode: snap so the
                // voice starts exactly on target. Every block after that: smooth
                // setters - per-block snapToPosition bypasses the slerp smoothing and
                // turns per-tap motion into audible block-rate clicks.
                if (tapVoicesNeedPositionSnap)
                    voiceProc->snapToPosition(tapAz[headIdx], tapEl[headIdx], tapDist[headIdx]);
                else
                {
                    voiceProc->setAzimuth(tapAz[headIdx]);
                    voiceProc->setElevation(tapEl[headIdx]);
                    voiceProc->setDistance(tapDist[headIdx]);
                }

                float* channelPtrs[2];
                channelPtrs[0] = headPostToneBuffers[(size_t)headIdx][0].data();
                if (NumChannels > 1)
                    channelPtrs[1] = headPostToneBuffers[(size_t)headIdx][1].data();
                juce::AudioBuffer<float> headAudioBuffer(channelPtrs, std::min(2, NumChannels), numSamples);
                voiceProc->processBlock(headAudioBuffer);

                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                    for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                        spatializedHeadsSum[ch][s] += headPostToneBuffers[(size_t)headIdx][ch][s];
            }

            // Replace echo output with the sum of the spatialized canonical taps.
            copyBuffer(spatializedHeadsSum, workBuffer, numSamples);
            tapVoicesNeedPositionSnap = false;
        }

        // Apply echo output level after tone stack (or spatial routing if Independent)
        for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
            for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                workBuffer[ch][s] *= DelayAmount;

        // Step 5b: Echo -> Reverb routing
        if (routingMode == 1)
        {
            if (ReverbEnabled && ReverbAmount > 0.0f)
            {
                const float echoToReverbGain = 1.00f;
                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                    for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
                        reverbbuffer[ch][s] = echoPostToneBuffer[ch][s] * echoToReverbGain;

                processReverbBuffer();
            }
        }

        // Step 6: Mix (linear dry/wet crossfade; equal-power echo/reverb branch tilt).
        const bool useReverb = (ReverbEnabled && ReverbAmount > 0.0f);
        for (int s = 0; s < numSamples; ++s)
        {
            const float mix = wetMixSmoother.getNextValue();
            const float branchBalance = branchBalanceSmoother.getNextValue();
            const float balanceAngle = juce::jlimit(0.0f, 1.0f, branchBalance) * juce::MathConstants<float>::halfPi;
            dryGainRamp[(size_t)s] = 1.0f - mix;
            wetGainRamp[(size_t)s] = mix;
            echoGainRamp[(size_t)s] = std::cos(balanceAngle);
            reverbGainRamp[(size_t)s] = std::sin(balanceAngle);
            echoLevelRamp[(size_t)s] = echoLevelSmoother.getNextValue();
            reverbLevelRamp[(size_t)s] = reverbLevelSmoother.getNextValue();
        }

        // The auto-panic runaway detector runs after this block has already
        // applied wet mix, branch balance, Echo Level, and the fixed delay
        // presentation trim. Publish the largest such legitimate gain so the
        // watchdog can normalize it back out and judge actual loop growth.
        autoPanicRunawayMonitorNormalization = 1.0f;
        for (int s = 0; s < numSamples; ++s)
        {
            const float wetGain = wetGainRamp[(size_t)s];
            const float echoPresentationGain =
                DelayAmount * wetGain * std::abs(echoGainRamp[(size_t)s]) * echoLevelRamp[(size_t)s];
            const float reverbPresentationGain =
                wetGain * std::abs(reverbGainRamp[(size_t)s]) * reverbLevelRamp[(size_t)s];
            autoPanicRunawayMonitorNormalization =
                std::max(autoPanicRunawayMonitorNormalization, std::max(echoPresentationGain, reverbPresentationGain));
        }

        const bool duckReverb = useReverb && ReverbDuckAmount > 1.0e-4f;
        const float duckAttackCoeff = onePoleCoeff(kReverbDuckAttackSeconds, SampleRate);
        const float duckReleaseCoeff = onePoleCoeff(kReverbDuckReleaseSeconds, SampleRate);
        const float duckMinGain = std::pow(10.0f, (kReverbDuckMaxAttenuationDb * ReverbDuckAmount) / 20.0f);

        float echoPeakLinear = 0.0f;
        float reverbPeakLinear = 0.0f;
        SPATIAL_ECHO_MEASURE_STAGE(mixUs, maxMixUs, {
            for (int s = 0; s < numSamples; ++s)
            {
                float reverbDuckGain = 1.0f;
                if (duckReverb)
                {
                    float detector = 0.0f;
                    for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                        detector = std::max(detector, std::abs(drybuffer[ch][static_cast<size_t>(s)]));

                    const float coeff = detector > reverbDuckEnvelope ? duckAttackCoeff : duckReleaseCoeff;
                    reverbDuckEnvelope = detector + coeff * (reverbDuckEnvelope - detector);
                    const float overThreshold = juce::jlimit(
                        0.0f, 1.0f, (reverbDuckEnvelope - kReverbDuckThreshold) / (1.0f - kReverbDuckThreshold));
                    const float duckCurve = std::sqrt(overThreshold);
                    reverbDuckGain = 1.0f - duckCurve * (1.0f - duckMinGain);
                }
                else
                {
                    reverbDuckEnvelope *= duckReleaseCoeff;
                }

                for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
                {
                    const float echoGain = echoGainRamp[(size_t)s];
                    const float reverbGain = reverbGainRamp[(size_t)s];
                    const float echoLevel = echoLevelRamp[(size_t)s];
                    const float reverbLevel = reverbLevelRamp[(size_t)s];
                    const float dryGain = dryGainRamp[(size_t)s];
                    const float wetGain = wetGainRamp[(size_t)s];
                    const float wetEcho = workBuffer[ch][static_cast<size_t>(s)] * echoGain * echoLevel;
                    const float wetReverb = useReverb ? (reverbRecoveredBuffer[ch][static_cast<size_t>(s)] *
                                                         reverbDuckGain * reverbGain * reverbLevel)
                                                      : 0.0f;
                    const float wetCombined = wetEcho + wetReverb;
                    workBuffer[ch][static_cast<size_t>(s)] =
                        drybuffer[ch][static_cast<size_t>(s)] * dryGain + wetCombined * wetGain;

                    const float wetEchoOut = std::abs(wetEcho * wetGain);
                    const float wetReverbOut = std::abs(wetReverb * wetGain);
                    echoPeakLinear = std::max(echoPeakLinear, wetEchoOut);
                    reverbPeakLinear = std::max(reverbPeakLinear, wetReverbOut);
                }
            }
        });

        // Fast UI peak-envelope ballistics: 180 ms release. This is
        // intentionally neither VU nor PPM behavior.
        const float releaseTimeSec = 0.18f;
        const float release = (SampleRate > 0.0f && numSamples > 0)
                                  ? std::exp(-static_cast<float>(numSamples) / (releaseTimeSec * SampleRate))
                                  : 0.0f;
        echoMeterEnvelope = juce::jmax(echoPeakLinear, echoMeterEnvelope * release);
        reverbMeterEnvelope = juce::jmax(reverbPeakLinear, reverbMeterEnvelope * release);
        echoBranchMeterLinear.store(echoMeterEnvelope, std::memory_order_relaxed);
        reverbBranchMeterLinear.store(reverbMeterEnvelope, std::memory_order_relaxed);

        // Step 7: Spatial routing - After Effects mode (mode 0, default)
        // Applies binaural after the wet/dry mix. Standalone output hiss is added later
        // so it remains a local stereo tape-machine bed in both spatial routes.
        // SKIP if Independent mode already applied per-tap binaural in Step 5c.
        if (SpatialRoutingMode == 0 && !doIndependentSpatialRouting)
        {
            SPATIAL_ECHO_MEASURE_STAGE(binauralUs, maxBinauralUs, { applyBinauralToWorkBuffer(); });
        }

        // Step 8: Add output noise as a true per-sample stereo bed (not block-held).
        const float fallbackOutputNoise = TapeDelay->getOutputNoise();
        SPATIAL_ECHO_MEASURE_STAGE(noiseUs, maxNoiseUs, {
            // Keep consuming the generator's fade after noise is disabled.
            // Settled-off buffers contain exact zeros.
            {
                for (int ch = 0; ch < NumChannels; ++ch)
                {
                    const auto& outputNoiseBuffer = TapeDelay->getOutputNoiseBufferForChannel(ch);
                    const int outputNoiseCount = static_cast<int>(outputNoiseBuffer.size());
                    for (int s = 0; s < numSamples; ++s)
                    {
            const float outputNoise =
                (s < outputNoiseCount) ? outputNoiseBuffer[static_cast<size_t>(s)] : fallbackOutputNoise;
            const float wetGain = wetGainRamp[(size_t)s];
            if (wetGain > 0.0f)
                workBuffer[static_cast<size_t>(ch)][static_cast<size_t>(s)] += outputNoise * wetGain;
                    }
    }
}
});

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
// Auto-panic test fault: simulate a broken stage emitting garbage onto
// the post-mix bus (see EchoesModelTestAccess::setWetBusFault).
if (!juce::exactlyEqual(debugWetBusFault, 0.0f))
    for (size_t ch = 0; ch < static_cast<size_t>(NumChannels); ++ch)
        for (size_t s = 0; s < static_cast<size_t>(numSamples); ++s)
            workBuffer[ch][s] += debugWetBusFault;

if (captureTiming)
{
    const auto blockUs = toMicros(StageClock::now() - blockStart);
    stageTiming.totalUs.fetch_add(blockUs, std::memory_order_relaxed);
    stageTiming.blocks.fetch_add(1, std::memory_order_relaxed);
    updateMaxAtomic(stageTiming.maxBlockUs, blockUs);
}
#endif

#undef SPATIAL_ECHO_MEASURE_STAGE
}
;

if (canProcessInPlace)
{
    processBlockInternal(buffer, availableSamples);
    return;
}

// Fallback: chunked processing into internal buffer to avoid RT allocations.
int offset = 0;
while (offset < availableSamples)
{
    const int chunk = std::min(maxChunk, availableSamples - offset);

    for (int ch = 0; ch < targetChannels; ++ch)
    {
        const int srcCh = std::min(ch, inputChannels - 1);
        std::memcpy(processingBuffer[static_cast<size_t>(ch)].data(),
                    buffer[static_cast<size_t>(srcCh)].data() + offset, sizeof(float) * (size_t)chunk);
    }

    processBlockInternal(processingBuffer, chunk);

    const int copyChannels = std::min(inputChannels, targetChannels);
    for (size_t ch = 0; ch < static_cast<size_t>(copyChannels); ++ch)
    {
        std::memcpy(buffer[ch].data() + offset, processingBuffer[ch].data(), sizeof(float) * (size_t)chunk);
    }

    offset += chunk;
}
}

void EchoesModel::autoPanicFlush(bool fadeOut)
{
    if (fadeOut)
    {
        // Finite runaway: the existing kill fade drains the tape loop. Reverb
        // uses a hard safety clear here, so the current tail may be truncated.
        TapeDelay->triggerKill();
        Reverb->hardClearNow();
        return;
    }

    // Non-finite state: hard clear now. Every recursive filter that saw the
    // NaN must be zeroed in the same block, or it re-poisons the cleared
    // buffers (an IIR fed clean input never recovers from a NaN'd state).
    TapeDelay->clearBuffers();
    Reverb->hardClearNow();
    for (auto& toneStack : ToneStacks)
        toneStack->clearAudioState();
    Preamp->clearAudioState();
    if (spatialField)
        spatialField->clearNaNLatchableState();
}

void EchoesModel::setTapeRecordMakeupGainDb(float makeupGainDb)
{
    if (TapeDelay)
        TapeDelay->setTapeRecordMakeupGainDb(makeupGainDb);
}
