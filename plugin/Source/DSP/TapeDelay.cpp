// TapeDelayProcessor: Multi-head tape echo with configurable head ratios and feedback.
// Realtime-safety: No allocation/locks in process()

#include "TapeDelay.h"
#include "DeterministicTestMode.h"

void TapeDelayProcessor::setHeadRatioPreset(int preset)
{
    HeadRatioPreset = preset;
    switch (preset)
    {
        case 0:  // Classic (1:1.901:2.751)
            HEAD2_RATIO = HeadRatios::CLASSIC_RATIO_2;
            HEAD3_RATIO = HeadRatios::CLASSIC_RATIO_3;
            break;
        case 1:  // Idealized (1:2:3) - simplified mathematical ratio
            HEAD2_RATIO = HeadRatios::IDEALIZED_RATIO_2;
            HEAD3_RATIO = HeadRatios::IDEALIZED_RATIO_3;
            break;
        case 2:  // Golden Ratio (1:1.618:2.618)
            HEAD2_RATIO = HeadRatios::GOLDEN_RATIO_2;
            HEAD3_RATIO = HeadRatios::GOLDEN_RATIO_3;
            break;
        case 3:  // Linear (1:1.5:2)
            HEAD2_RATIO = HeadRatios::LINEAR_RATIO_2;
            HEAD3_RATIO = HeadRatios::LINEAR_RATIO_3;
            break;
        default:
            HEAD2_RATIO = HeadRatios::CLASSIC_RATIO_2;
            HEAD3_RATIO = HeadRatios::CLASSIC_RATIO_3;
            break;
    }
}

TapeDelayProcessor::TapeDelayProcessor()
{
}

TapeDelayProcessor::~TapeDelayProcessor()
{
    // Nothing to clean up
}

void TapeDelayProcessor::Reset(float sampleRate, int numChannels, int maxBlockSize)
{
    initEnvelopeLuts();

    // Clear previous DSP objects
    TapeBuffer.clear();
    BallisticsFilters.clear();
    WFProcessors.clear();
    FeedbackLPfilters.clear();
    FeedbackHPfilters.clear();

    // Assign plugin variables to object
    NumChannels = numChannels;
    SampleRate = sampleRate;
    delayTimePrimed = false;
    IntensitySmoothed = Intensity;  // Initialize smoother to current value
    intensitySmoothCoeff = (sampleRate == 48000.0f)
                               ? 0.995f
                               : std::exp(-1.0f / (INTENSITY_SMOOTH_TAU_SECONDS * std::max(1.0f, sampleRate)));
    // Record filters run at the host rate.
    recordChainSampleRate = sampleRate;
    recordSaturator.prepare(sampleRate, numChannels);
    // Both coefficients derive from 41.7 ms time constants so the windows hold
    // at every sample rate (a fixed 0.9995 per-sample coefficient would shrink
    // to 10.4 ms at 192 kHz); 48 kHz yields exactly 0.9995f.
    feedbackAvgCoeff = onePoleCoeffForTau(FEEDBACK_AVG_TAU_SECONDS, sampleRate, 0.9995f);
    tempoSyncGlideCoeff = onePoleCoeffForTau(TEMPO_SYNC_GLIDE_TAU_SECONDS, sampleRate, 0.9995f);
    feedbackAvgLevel[0] = 0.0f;
    feedbackAvgLevel[1] = 0.0f;
    const float modLpfFc = 90.0f;
    wfModLPCoeff = 1.0f - std::exp(-2.0f * 3.14159f * modLpfFc / std::max(1.0f, SampleRate));
    const float maxDeltaSamples = 0.05f;
    wfModMaxDeltaMs = (1000.0f * maxDeltaSamples) / std::max(1.0f, SampleRate);
    wfModLPState[0] = wfModLPState[1] = 0.0f;
    wfModSlewState[0] = wfModSlewState[1] = 0.0f;

    auto timeMsToCoeff = [sampleRate](float timeMs)
    {
        const float timeSamples = std::max(1.0f, sampleRate * (timeMs * 0.001f));
        return 1.0f / timeSamples;
    };
    loopNoiseGateAttackCoeff = timeMsToCoeff(4.0f);
    loopNoiseGateReleaseCoeff = timeMsToCoeff(120.0f);
    resetLoopNoiseGate();

    // Self-oscillation heat envelope: ~5 ms attack, ~300 ms release. Fast enough to
    // catch the loop blooming, slow enough that the PANIC light holds while it
    // sustains and eases back down once the oscillation stops or panic clears it.
    {
        const float heatAttackSec = 0.005f;
        const float heatReleaseSec = 0.30f;
        selfOscHeatAttackCoeff = 1.0f - std::exp(-1.0f / std::max(1.0f, heatAttackSec * SampleRate));
        selfOscHeatReleaseCoeff = 1.0f - std::exp(-1.0f / std::max(1.0f, heatReleaseSec * SampleRate));
    }
    selfOscHeatEnv = 0.0f;
    selfOscHeatPublished.store(0.0f, std::memory_order_relaxed);

    // Always false in a shipping Release plug-in; see DeterministicTestMode.h.
    const bool deterministic = SpatialEchoDeterminism::isDeterministicTestMode();
    constexpr uint32_t seedBase = SpatialEchoDeterminism::kSeedBase;

    // Reset tape noise processor
    NoiseProcessor.reset(sampleRate);
    ReverbNoiseProcessor.reset(sampleRate);
    preparedReverbNoiseSamples = 0;
    tapeNoiseActivityForEnvelope = 0.0f;
    // Keep this preallocated to avoid resizing on the audio thread.
    const int noiseBlockCapacity = std::max(1, maxBlockSize);
    outputNoisePerSample.assign(static_cast<size_t>(noiseBlockCapacity), 0.0f);
    outputNoisePerChannel.assign(static_cast<size_t>(std::max(1, numChannels)),
                                 std::vector<float>(static_cast<size_t>(noiseBlockCapacity), 0.0f));
    reverbNoisePerChannel.assign(static_cast<size_t>(std::max(1, numChannels)),
                                 std::vector<float>(static_cast<size_t>(noiseBlockCapacity), 0.0f));
    if (deterministic)
    {
        NoiseProcessor.setDeterministicSeed(seedBase ^ 0x13579BDFu);
        ReverbNoiseProcessor.setDeterministicSeed(seedBase ^ 0x2468ACE0u);
    }

    // Initialize record chain filter coefficients (single record point model)
    // One-pole filter: coeff = exp(-2*pi*fc/fs)

    // Fixed, wide conditioning before the nonlinear record point. Audible
    // speed/condition tone belongs to playback; progressive tone belongs to
    // the dedicated feedback filter.
    recordFilterCutoff =
        std::min(RECORD_ANTI_ALIAS_CUTOFF_HZ, RECORD_ANTI_ALIAS_NYQUIST_FRACTION * recordChainSampleRate);
    recordLPF_coeff = std::exp(-2.0f * 3.14159f * recordFilterCutoff / recordChainSampleRate);
    recordLPF_state[0] = 0.0f;
    recordLPF_state[1] = 0.0f;
    recordLPF2_state[0] = 0.0f;
    recordLPF2_state[1] = 0.0f;

    // HPF: ~60Hz cutoff to prevent LF buildup
    float recordHPF_fc = 60.0f;
    recordHPF_coeff = std::exp(-2.0f * 3.14159f * recordHPF_fc / recordChainSampleRate);
    recordHPF_state[0] = 0.0f;
    recordHPF_state[1] = 0.0f;

    // DC blocker: ~30Hz to remove saturation-induced DC offset
    float dcBlock_fc = 30.0f;
    recordDCBlock_coeff = std::exp(-2.0f * 3.14159f * dcBlock_fc / recordChainSampleRate);
    recordDCBlock_state[0] = 0.0f;
    recordDCBlock_state[1] = 0.0f;

    // Initialize tape splice simulation (phase accumulator model)
    tapePhase = 0.0f;
    splicePhaseWidth = 0.01f;  // ~1% of loop is splice zone

    // A kill fade left running across a prepareToPlay would keep multiplying
    // the new stream by a stale envelope and would advance it with a step
    // computed from the previous sample rate. Reset() is a full restart.
    killActive = false;
    killEnvelope = 0.0f;
    killStep = (sampleRate > 0.0f) ? (1.0f / (KILL_FADE_TIME_SEC * sampleRate)) : 1.0f;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    headTimingCaptured = false;
    headTimingSnapshot = HeadTimingSnapshot{};
    headTimingSampleCounter = 0;
#endif

    // Initialize mode crossfade (prevents clicks during mode switches)
    crossfadeStep = (sampleRate > 0.0f) ? (1.0f / (CROSSFADE_TIME_SEC * sampleRate)) : 1.0f;
    crossfadeActive = false;
    crossfadeProgress = 1.0f;
    pingPongSeedAngle = PING_PONG_BALANCED_ROTATION;
    pingPongSeedTargetAngle = PING_PONG_BALANCED_ROTATION;
    pingPongSeedAngleStep = 0.0f;
    pingPongSeedCos = std::cos(PING_PONG_BALANCED_ROTATION);
    pingPongSeedSin = std::sin(PING_PONG_BALANCED_ROTATION);
    pingPongSeedPhasorRenormalizeRemaining = PING_PONG_PHASOR_RENORMALIZE_INTERVAL;
    pingPongSeedRampSamples =
        std::max(1, static_cast<int>(std::lround(std::max(1.0f, sampleRate) * PING_PONG_SEED_SLEW_TIME_SEC)));
    pingPongSeedRampRemaining = 0;
    pingPongActivityReleaseSamples =
        std::max(1, static_cast<int>(std::lround(std::max(1.0f, sampleRate) * PING_PONG_ACTIVITY_RELEASE_TIME_SEC)));
    pingPongActivitySamplesRemaining = 0;
    pingPongSeedAngleInitialized = false;

    // Initialize per-head smoothed delays
    Head1SyncSmoothed = 0.0f;
    Head2SyncSmoothed = 0.0f;
    Head3SyncSmoothed = 0.0f;

    // Initialize tempo sync crossfade state
    syncCrossfadeActive[0] = false;
    syncCrossfadeActive[1] = false;
    syncCrossfadeActive[2] = false;
    syncCrossfadeProgress[0] = 0.0f;
    syncCrossfadeProgress[1] = 0.0f;
    syncCrossfadeProgress[2] = 0.0f;
    syncCrossfadePrevMs[0] = 0.0f;
    syncCrossfadePrevMs[1] = 0.0f;
    syncCrossfadePrevMs[2] = 0.0f;
    syncCrossfadeTargetMs[0] = 0.0f;
    syncCrossfadeTargetMs[1] = 0.0f;
    syncCrossfadeTargetMs[2] = 0.0f;
    syncCrossfadeStep = (sampleRate > 0.0f) ? (1.0f / (SYNC_CROSSFADE_TIME_SEC * sampleRate)) : 0.0f;

    // Initialize head enable smoothing
    headEnableInitialized = false;
    headEnableGain[0] = 0.0f;
    headEnableGain[1] = 0.0f;
    headEnableGain[2] = 0.0f;
    headEnableTarget[0] = 0.0f;
    headEnableTarget[1] = 0.0f;
    headEnableTarget[2] = 0.0f;
    headEnableStep = (sampleRate > 0.0f) ? (1.0f / (HEAD_ENABLE_FADE_TIME_SEC * sampleRate)) : 0.0f;

    // Initialize echo send / delay enable smoothing
    DelayEnabledInitialized = false;
    DelayEnabledGain = 0.0f;
    DelayEnabledTarget = 0.0f;
    delayEnableStep = (sampleRate > 0.0f) ? (1.0f / (DELAY_ENABLE_FADE_TIME_SEC * sampleRate)) : 0.0f;

    EchoSendInitialized = false;
    EchoSendGain = 0.0f;
    EchoSendTarget = 0.0f;
    echoSendStep = (sampleRate > 0.0f) ? (1.0f / (ECHO_SEND_FADE_TIME_SEC * sampleRate)) : 0.0f;

    PreviousStereoMode = StereoMode;

    // Initialize per-head capture buffers (for Independent mode spatial routing)
    perHeadCaptureEnabled = false;
    for (size_t h = 0; h < 3; ++h)
    {
        for (size_t ch = 0; ch < 2; ++ch)
            perHeadCaptureBuffers[h][ch].assign(static_cast<size_t>(maxBlockSize), 0.0f);
    }

    for (int channel = 0; channel < numChannels; channel++)
    {
        // Initialise DSP objects
        // SINGLE BUFFER per channel - reads at 3 tap points (shared-tape architecture)
        TapeBuffer.push_back(std::make_unique<CircularBuffer>());
        BallisticsFilters.push_back(std::make_unique<OnePole>());
        WFProcessors.push_back(std::make_unique<WowFlutterSignalModel>());

        // Reset circular buffer
        // Buffer must accommodate longest head delay (Head 3 at ~3x base time)
        // At 40 BPM whole note: 6000ms * 3 = 18000ms + margin for wow/flutter
        TapeBuffer[static_cast<size_t>(channel)]->Init(static_cast<int>(sampleRate), 19500.0f);

        // Reset control ballistics filter
        BallisticsFilters[static_cast<size_t>(channel)]->reset(sampleRate);

        // Design: Asymmetric motor ballistics for classic tape-echo behavior.
        // Slowing down (longer delay) takes longer than speeding up (shorter delay).
        // 800ms up (increase delay), 400ms down (decrease delay). Do not change ratios.
        float timeUp = 0.8f;    // Design: 800ms to slow down (increase delay time)
        float timeDown = 0.4f;  // Design: 400ms to speed up (decrease delay time)
        BallisticsFilters[static_cast<size_t>(channel)]->setAlphaAsymmetric(timeUp, timeDown);

        // Reset wow and flutter object
        WFProcessors[static_cast<size_t>(channel)]->init();
        if (deterministic)
        {
            const uint32_t wfSeed = seedBase + 0x1000u + static_cast<uint32_t>(channel) * 0x101u;
            WFProcessors[static_cast<size_t>(channel)]->setDeterministicSeed(wfSeed);
        }
        WFProcessors[static_cast<size_t>(channel)]->reset(sampleRate);

        // Initialize feedback path filters - KEY FOR PROGRESSIVE DARKENING
        // These are applied to feedback BEFORE it's written to the buffer,
        // so each repeat gets progressively filtered (darker)
        FeedbackLPfilters.push_back(std::make_unique<RbjBiquad>());
        FeedbackHPfilters.push_back(std::make_unique<RbjBiquad>());
        FeedbackLPfilters[static_cast<size_t>(channel)]->Init(BiquadType::LPF, 8000.f, sampleRate, 0.5f, 0.f);
        FeedbackLPfilters[static_cast<size_t>(channel)]->Reset(sampleRate);
        FeedbackHPfilters[static_cast<size_t>(channel)]->Init(BiquadType::HPF, 80.f, sampleRate, 0.5f, 0.0f);
        FeedbackHPfilters[static_cast<size_t>(channel)]->Reset(sampleRate);
    }

    for (size_t head = 0; head < 3; ++head)
    {
        for (size_t channel = 0; channel < 2; ++channel)
        {
            perHeadPlaybackHPFilters[head][channel].Init(BiquadType::HPF, PLAYBACK_HP_CUTOFF_HZ, sampleRate,
                                                         PLAYBACK_HP_Q, 0.0f);
            perHeadPlaybackHPFilters[head][channel].Reset(sampleRate);
            perHeadPlaybackLPFilters[head][channel].Init(BiquadType::LPF, 9000.f, sampleRate, 0.5f, 0.f);
            perHeadPlaybackLPFilters[head][channel].Reset(sampleRate);
            perHeadBumpFilters[head][channel].Init(BiquadType::Peaking, 80.f, sampleRate, 1.5f, 6.0f);
            perHeadBumpFilters[head][channel].Reset(sampleRate);
        }
    }
}

void TapeDelayProcessor::UpdateParameters(float intensity, float delaytime, int playheadEnabled[], int delayEnabled,
                                          float echoSend, bool noiseEnabled, float noiseAmount, float tapeCondition,
                                          float wowFlutterDepth, float wowRate, float flutterRate,
                                          bool tempoSyncEnabled, float head1SyncDelayMs, float head2SyncDelayMs,
                                          float head3SyncDelayMs, int stereoMode, int headRatioPreset,
                                          bool feedbackCompEnabled, float presetAsperity, float presetDropout,
                                          float presetScrapeScale, float presetLpfScale, float feedbackUserIntensity)
{
    // Save plugin parameters to object
    Intensity = intensity;
    feedbackCompensationEnabled = feedbackCompEnabled;

    // Apply head ratio preset (user-configurable via Advanced menu)
    setHeadRatioPreset(headRatioPreset);

    // DelayTime is only used in Classic mode (tempo sync uses per-head values in ProcessBuffer)
    // Classic mode: standard 175-70ms mapping
    // Knob behavior: 0 (CCW) = longest delay (175ms), 1 (CW) = shortest delay (70ms)
    DelayTime = map(delaytime, 0.f, 1.f, 175.f, 70.f);
    DelayEnabled = delayEnabled;
    EchoSend = echoSend;

    DelayEnabledTarget = (DelayEnabled != 0) ? 1.0f : 0.0f;
    // Keep delay disable deterministic/click-safe: when delay is disabled, force input send to zero.
    EchoSendTarget = ((DelayEnabled != 0) && (EchoSend > 0.5f)) ? 1.0f : 0.0f;
    if (!DelayEnabledInitialized)
    {
        DelayEnabledGain = DelayEnabledTarget;
        DelayEnabledInitialized = true;
    }
    if (!EchoSendInitialized)
    {
        EchoSendGain = EchoSendTarget;
        EchoSendInitialized = true;
    }

    const bool tempoSyncWasEnabled = TempoSyncEnabled;
    const float prevHead1SyncDelayMs = Head1SyncDelayMs;
    const float prevHead2SyncDelayMs = Head2SyncDelayMs;
    const float prevHead3SyncDelayMs = Head3SyncDelayMs;
    TempoSyncEnabled = tempoSyncEnabled;

    // Store per-head tempo sync delays
    Head1SyncDelayMs = head1SyncDelayMs;
    Head2SyncDelayMs = head2SyncDelayMs;
    Head3SyncDelayMs = head3SyncDelayMs;

    if (!TempoSyncEnabled)
    {
        Head1SyncSmoothed = 0.0f;
        Head2SyncSmoothed = 0.0f;
        Head3SyncSmoothed = 0.0f;
        syncCrossfadeActive[0] = false;
        syncCrossfadeActive[1] = false;
        syncCrossfadeActive[2] = false;
        syncCrossfadeProgress[0] = 0.0f;
        syncCrossfadeProgress[1] = 0.0f;
        syncCrossfadeProgress[2] = 0.0f;
    }
    else if (!tempoSyncWasEnabled)
    {
        // First enable: jump directly to target
        Head1SyncSmoothed = Head1SyncDelayMs;
        Head2SyncSmoothed = Head2SyncDelayMs;
        Head3SyncSmoothed = Head3SyncDelayMs;
        syncCrossfadeActive[0] = false;
        syncCrossfadeActive[1] = false;
        syncCrossfadeActive[2] = false;
        syncCrossfadeProgress[0] = 0.0f;
        syncCrossfadeProgress[1] = 0.0f;
        syncCrossfadeProgress[2] = 0.0f;
        syncCrossfadePrevMs[0] = Head1SyncDelayMs;
        syncCrossfadePrevMs[1] = Head2SyncDelayMs;
        syncCrossfadePrevMs[2] = Head3SyncDelayMs;
        syncCrossfadeTargetMs[0] = Head1SyncDelayMs;
        syncCrossfadeTargetMs[1] = Head2SyncDelayMs;
        syncCrossfadeTargetMs[2] = Head3SyncDelayMs;
    }
    else
    {
        auto maybeStartSyncCrossfade = [&](int idx, float prevDelay, float newDelay, float smoothedDelay)
        {
            if (!syncCrossfadeActive[idx] && std::abs(newDelay - prevDelay) >= SYNC_CROSSFADE_THRESHOLD_MS &&
                newDelay > 1.0f)
            {
                syncCrossfadeActive[idx] = true;
                syncCrossfadeProgress[idx] = 0.0f;
                syncCrossfadePrevMs[idx] = (smoothedDelay > 1.0f ? smoothedDelay : prevDelay);
                syncCrossfadeTargetMs[idx] = newDelay;
            }
        };

        maybeStartSyncCrossfade(0, prevHead1SyncDelayMs, Head1SyncDelayMs, Head1SyncSmoothed);
        maybeStartSyncCrossfade(1, prevHead2SyncDelayMs, Head2SyncDelayMs, Head2SyncSmoothed);
        maybeStartSyncCrossfade(2, prevHead3SyncDelayMs, Head3SyncDelayMs, Head3SyncSmoothed);
    }

    // Detect stereo mode changes and trigger crossfade to prevent clicks
    const int clampedStereoMode = (stereoMode != 0) ? 1 : 0;
    if (clampedStereoMode != StereoMode)
    {
        const bool reversesActiveCrossfade = crossfadeActive && clampedStereoMode == PreviousStereoMode;
        crossfadeActive = true;
        if (reversesActiveCrossfade)
            crossfadeProgress = std::clamp(1.0f - crossfadeProgress, 0.0f, 1.0f);
        else
            crossfadeProgress = 0.0f;

        PreviousStereoMode = StereoMode;  // Store old mode for crossfade blending
    }
    StereoMode = clampedStereoMode;

    // Store playhead states
    PH1enabled = playheadEnabled[0];
    PH2enabled = playheadEnabled[1];
    PH3enabled = playheadEnabled[2];

    // Smooth head enable changes (clickless toggles)
    headEnableTarget[0] = (PH1enabled != 0) ? 1.0f : 0.0f;
    headEnableTarget[1] = (PH2enabled != 0) ? 1.0f : 0.0f;
    headEnableTarget[2] = (PH3enabled != 0) ? 1.0f : 0.0f;
    if (!headEnableInitialized)
    {
        headEnableGain[0] = headEnableTarget[0];
        headEnableGain[1] = headEnableTarget[1];
        headEnableGain[2] = headEnableTarget[2];
        headEnableInitialized = true;
    }

    const float transportTargetDelayMs = getTransportTargetDelayMs();
    if (!delayTimePrimed && !BallisticsFilters.empty())
    {
        for (auto& filter : BallisticsFilters)
        {
            if (filter)
                filter->setCurrentValue(transportTargetDelayMs);
        }
        delayTimePrimed = true;
    }

    // The per-head delay arguments are ignored. Physical head spacing fixes the
    // timing ratios per the selected head-ratio preset in HeadRatioConstants.h
    // (Classic 1:1.901:2.751 artistic, Idealized 1:2:3 RE-201, Golden phi,
    // Linear 1:1.5:2) - never a fixed 2x/3x.

    // Design: TapeCondition darkens worn tape, but the first repeat must stay
    // recognisably present. A 5-15 kHz range plus long-delay collapse makes the
    // first repeat too dull before the user even hears the feedback character.
    const float baseMin = PLAYBACK_SLOW_CUTOFF_HZ;
    const float baseMax = PLAYBACK_FAST_CUTOFF_HZ;
    const float conditionClamped = std::clamp(tapeCondition, 0.0f, 1.0f);
    const float conditionToneShaped = std::pow(conditionClamped, 0.8f);
    const float ageFactor = 1.0f - PLAYBACK_CONDITION_DEPTH * conditionToneShaped;
    float minCutoff = baseMin * ageFactor;
    float maxCutoff = baseMax * ageFactor;

    const float delayForFilter = std::clamp(transportTargetDelayMs, 70.0f, 6000.0f);
    const float classicSpeedNorm =
        juce::jlimit(0.0f, 1.0f, ((175.0f / std::min(delayForFilter, 175.0f)) - 1.0f) / 1.5f);
    float cutoff = minCutoff + (maxCutoff - minCutoff) * classicSpeedNorm;
    cutoff = std::clamp(cutoff, 5000.0f, 19000.0f);
    playbackFilterCutoff = cutoff;

    for (size_t head = 0; head < 3; ++head)
        for (size_t channel = 0; channel < 2; ++channel)
            perHeadPlaybackLPFilters[head][channel].SetFc(cutoff);

    // Feedback tone is independent from the playback cutoff. Tape flavor
    // chooses how quickly repeats darken; delay time and playback contour do
    // not move this filter.
    const float feedbackConditionFactor = 1.0f - FEEDBACK_CONDITION_DEPTH * conditionToneShaped;
    const float feedbackFlavorScale = std::clamp(presetLpfScale, 0.8f, 1.2f);
    feedbackFilterCutoff = FEEDBACK_BASE_CUTOFF_HZ * feedbackConditionFactor * feedbackFlavorScale;
    feedbackFilterCutoff = std::clamp(feedbackFilterCutoff, FEEDBACK_MIN_CUTOFF_HZ, FEEDBACK_MAX_CUTOFF_HZ);
    const size_t feedbackFilterChannels =
        std::min(static_cast<size_t>(std::max(0, NumChannels)), FeedbackLPfilters.size());
    for (size_t channel = 0; channel < feedbackFilterChannels; ++channel)
    {
        FeedbackLPfilters[channel]->SetFc(feedbackFilterCutoff);
    }

    // Noise follows the normalized public Feedback domain, not the calibrated
    // loop gain (which intentionally exceeds unity near the top of the knob).
    const float fallbackNoiseIntensity = std::clamp(std::isfinite(intensity) ? intensity : 0.0f, 0.0f, 1.0f);
    const float noiseFeedbackIntensity = std::isfinite(feedbackUserIntensity) && feedbackUserIntensity >= 0.0f
                                             ? std::clamp(feedbackUserIntensity, 0.0f, 1.0f)
                                             : fallbackNoiseIntensity;
    NoiseProcessor.updateParameters(noiseEnabled, noiseAmount, tapeCondition, noiseFeedbackIntensity, presetAsperity,
                                    presetDropout);
    ReverbNoiseProcessor.updateParameters(noiseEnabled, noiseAmount, tapeCondition, noiseFeedbackIntensity,
                                          presetAsperity, 0.0f);

    // Splice degradation uses a shaped curve so mid Condition values are still audible,
    // while preserving a stronger rise near worn extremes.
    float conditionCurve = std::pow(conditionClamped, 1.2f);  // 0.5 -> 0.44, 0.75 -> 0.71
    spliceIntensity = 0.36f * conditionCurve;                 // 0% pristine -> 36% worn

    // Splice width also increases with wear (adhesive loosens, tape stretches at joint)
    float spliceWidthFactor = 1.0f + 0.6f * conditionToneShaped;  // Worn tape = wider splice (1.0x to 1.6x)
    splicePhaseWidth = 0.01f * spliceWidthFactor;                 // ~1% to ~1.5% of loop

    // Splice-induced wow: when splice passes capstan, the thicker joint causes brief speed variation
    // UAD Galaxy docs: "the tape capstan (where it will create some wow and flutter)"
    // Amount scales with condition (worn splice = more mechanical disturbance)
    spliceWowAmount = 0.20f * conditionCurve;  // 0-0.2ms pitch deviation

    // Loop timing is computed per-sample in ProcessBuffer using the smoothed
    // motor state (delayTimeSmoothed), so splice timing ramps with motor ballistics
    // rather than stepping abruptly when parameters change.

    // Keep the record path as fixed anti-alias conditioning, not as another
    // audible time-, condition-, or flavor-dependent tone shaper.
    const float recordRate = (recordChainSampleRate > 0.0f) ? recordChainSampleRate : SampleRate;
    recordFilterCutoff = std::min(RECORD_ANTI_ALIAS_CUTOFF_HZ, RECORD_ANTI_ALIAS_NYQUIST_FRACTION * recordRate);
    recordLPF_coeff = std::exp(-2.0f * 3.14159f * recordFilterCutoff / recordRate);

    // Update wow/flutter parameters for all channels (scaled by tape condition)
    // Pass delay time for dynamic rate scaling
    for (int channel = 0; channel < NumChannels; channel++)
    {
        WFProcessors[static_cast<size_t>(channel)]->updateParameters(
            wowFlutterDepth, wowRate, flutterRate, tapeCondition, transportTargetDelayMs, presetScrapeScale);

        // Stereo decorrelation: apply a phase offset to the right channel
        // ~10% of 2*pi (0.628 radians) for subtle stereo width without obvious modulation difference
        if (channel == 1)
        {
            WFProcessors[static_cast<size_t>(channel)]->setStereoPhaseOffset(0.628f);  // ~36 degrees offset
        }
    }
}

void TapeDelayProcessor::prepareReverbNoiseBuffer(const std::vector<std::vector<float>>& input, int blockSize)
{
    preparedReverbNoiseSamples = 0;
    const int channels = std::min({2, static_cast<int>(input.size()), static_cast<int>(reverbNoisePerChannel.size())});
    if (blockSize <= 0 || channels <= 0)
        return;

    int availableSamples = blockSize;
    for (size_t channel = 0; channel < static_cast<size_t>(channels); ++channel)
    {
        availableSamples = std::min(availableSamples, static_cast<int>(input[channel].size()));
        availableSamples = std::min(availableSamples, static_cast<int>(reverbNoisePerChannel[channel].size()));
    }
    if (availableSamples < blockSize)
        jassertfalse;

    for (size_t sample = 0; sample < static_cast<size_t>(availableSamples); ++sample)
    {
        // Tank noise follows the current excitation without a stereo sum that
        // cancels anti-phase material. It has no dependency on future tape writes.
        float excitation = 0.0f;
        for (size_t channel = 0; channel < static_cast<size_t>(channels); ++channel)
        {
            const float value = input[channel][sample];
            if (std::isfinite(value))
                excitation = std::max(excitation, std::abs(value));
        }
        ReverbNoiseProcessor.processSample(excitation);
        for (int channel = 0; channel < channels; ++channel)
            reverbNoisePerChannel[static_cast<size_t>(channel)][sample] =
                ReverbNoiseProcessor.getReverbNoiseForChannel(channel);
    }
    preparedReverbNoiseSamples = availableSamples;
}

void TapeDelayProcessor::ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize)
{
    juce::ScopedNoDenormals noDenormals;

    const int bufferChannels = static_cast<int>(buffer.size());
    if (blockSize <= 0 || bufferChannels <= 0)
        return;

    const int preparedChannels = static_cast<int>(TapeBuffer.size());
    const int safeChannels =
        preparedChannels > 0 ? std::min(std::min(bufferChannels, preparedChannels), 2) : std::min(bufferChannels, 2);

    if (safeChannels != bufferChannels)
        jassertfalse;

    NumChannels = safeChannels;
    if (NumChannels <= 0)
        return;

    int processBlockSize = blockSize;
    for (size_t channel = 0; channel < static_cast<size_t>(NumChannels); ++channel)
        processBlockSize = std::min(processBlockSize, static_cast<int>(buffer[channel].size()));

    if (processBlockSize < blockSize)
        jassertfalse;
    if (processBlockSize <= 0)
        return;

    blockSize = processBlockSize;

    // EchoesModel prepares before its routing branch; standalone callers prepare
    // here. Consume the prefix once so the bed advances once per audio sample.
    const int reverbWriteLimit =
        reverbNoisePerChannel.empty() ? 0 : std::min(blockSize, static_cast<int>(reverbNoisePerChannel.front().size()));
    if (preparedReverbNoiseSamples != reverbWriteLimit)
        prepareReverbNoiseBuffer(buffer, blockSize);
    preparedReverbNoiseSamples = 0;

    const int noiseWriteLimit = std::min(blockSize, static_cast<int>(outputNoisePerSample.size()));
    if (noiseWriteLimit < blockSize)
        jassertfalse;
    if (noiseWriteLimit > 0)
        std::fill(outputNoisePerSample.begin(), outputNoisePerSample.begin() + noiseWriteLimit, 0.0f);
    for (auto& channelNoise : outputNoisePerChannel)
    {
        const int channelNoiseLimit = std::min(blockSize, static_cast<int>(channelNoise.size()));
        if (channelNoiseLimit > 0)
            std::fill(channelNoise.begin(), channelNoise.begin() + channelNoiseLimit, 0.0f);
    }

    // For stereo processing, we always process sample-by-sample across both channels
    // This allows stereo mode output routing to change instantly
    if (NumChannels >= 2)
    {
        // Persist the rotation phasor across callbacks. Reconstructing it from
        // the scalar angle at each callback boundary changes rounding during a
        // live slew and makes the waveform depend on host partition size.
        float seedCos = pingPongSeedCos;
        float seedSin = pingPongSeedSin;
        float seedStepCos = std::cos(pingPongSeedAngleStep);
        float seedStepSin = std::sin(pingPongSeedAngleStep);

        for (int sample = 0; sample < blockSize; sample++)
        {
            // Get input from both channels
            float inputL = buffer[0][static_cast<size_t>(sample)];
            float inputR = buffer[1][static_cast<size_t>(sample)];

            DelayEnabledGain = rampToward(DelayEnabledGain, DelayEnabledTarget, delayEnableStep);
            EchoSendGain = rampToward(EchoSendGain, EchoSendTarget, echoSendStep);

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
            if (captureHeadTiming)
                ++headTimingSampleCounter;
#endif

            // ================================================================
            // TAPE TRANSPORT: Single motor drives timing and tone
            // ================================================================
            // Single motor/capstan model: use channel 0's
            // ballistics as the canonical motor state for splice timing.
            // Both channels' head timing still gets per-channel W&F modulation.
            // ================================================================
            updateTempoSyncSmoothedDelays();
            float delayTimeSmoothedMotor = processTransportDelayMs(0);

            // Advance NoiseProcessor before read/write decisions so dropout state is current.
            float inputAvg = (inputL + inputR) * 0.5f;
            const float noiseEnvelopeInput = std::max(std::abs(inputAvg), tapeNoiseActivityForEnvelope);
            NoiseProcessor.processSample(noiseEnvelopeInput);
            if (sample < noiseWriteLimit)
            {
                outputNoisePerSample[static_cast<size_t>(sample)] = NoiseProcessor.getOutputNoise();
                if (outputNoisePerChannel.size() > 0)
                    outputNoisePerChannel[0][static_cast<size_t>(sample)] = NoiseProcessor.getOutputNoiseForChannel(0);
                if (outputNoisePerChannel.size() > 1)
                    outputNoisePerChannel[1][static_cast<size_t>(sample)] = NoiseProcessor.getOutputNoiseForChannel(1);
            }
            float dropoutGain = NoiseProcessor.getDropoutGain();

            // Get wow and flutter for both channels (independent per-channel variation)
            float WF_L = WFProcessors[0]->processSample();
            float WF_R = WFProcessors[1]->processSample();

            // Splice-induced wow burst: calculate using PREVIOUS tapePhase.
            // This adds pitch modulation when splice passes capstan.
            float spliceWowBurst = calculateSpliceWowBurst();

            // Add splice wow to wow/flutter modulation
            WF_L += spliceWowBurst;
            WF_R += spliceWowBurst;
            WF_L = shapeDelayModulation(WF_L, 0);
            WF_R = shapeDelayModulation(WF_R, 1);

            // Per-channel delay smoothing for W&F application (uses same target)
            float delayTimeSmoothedL = delayTimeSmoothedMotor;  // Use motor state
            float delayTimeSmoothedR = processTransportDelayMs(1);

            // Calculate head delay times
            // TEMPO SYNC: Per-head subdivision delays (user-configurable via heads display)
            // Each head can be set to any note division independently
            // Classic mode: Uses head ratios (1:1.901:2.751).

            // Validate delay times - clamp to safe range to prevent crackling from invalid values
            // Min 1ms to avoid interpolation issues, max based on buffer sizes
            auto clampDelay = [](float ms)
            {
                if (std::isnan(ms) || std::isinf(ms))
                    return 70.0f;  // Default to safe value
                return std::max(1.0f, std::min(ms, 6000.0f));
            };

            float baseL[3] = {0.0f, 0.0f, 0.0f};
            float baseR[3] = {0.0f, 0.0f, 0.0f};
            if (TempoSyncEnabled)
            {
                baseL[0] = Head1SyncSmoothed;
                baseL[1] = Head2SyncSmoothed;
                baseL[2] = Head3SyncSmoothed;
                baseR[0] = baseL[0];
                baseR[1] = baseL[1];
                baseR[2] = baseL[2];
            }
            else
            {
                // Design: Head ratios for classic spacing behavior.
                baseL[0] = delayTimeSmoothedL;
                baseL[1] = delayTimeSmoothedL * HEAD2_RATIO;
                baseL[2] = delayTimeSmoothedL * HEAD3_RATIO;
                baseR[0] = delayTimeSmoothedR;
                baseR[1] = delayTimeSmoothedR * HEAD2_RATIO;
                baseR[2] = delayTimeSmoothedR * HEAD3_RATIO;
            }

            float modL[3] = {0.0f, 0.0f, 0.0f};
            float modR[3] = {0.0f, 0.0f, 0.0f};
            for (int head = 0; head < 3; ++head)
            {
                modL[head] = scaleDelayModulationForHead(WF_L, baseL[head], delayTimeSmoothedL);
                modR[head] = scaleDelayModulationForHead(WF_R, baseR[head], delayTimeSmoothedR);
            }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
            if (captureHeadTiming && !headTimingCaptured)
            {
                const float inputPeak = std::max(std::abs(inputL), std::abs(inputR));
                if (inputPeak > 0.01f)
                {
                    headTimingCaptured = true;
                    headTimingSnapshot.delayTimeTargetMs = getTransportTargetDelayMs();
                    headTimingSnapshot.delayTimeSmoothedMs = delayTimeSmoothedL;
                    // Left-channel head times on the non-crossfade path.
                    headTimingSnapshot.h1TimeMs = clampDelay(baseL[0] + modL[0]);
                    headTimingSnapshot.h2TimeMs = clampDelay(baseL[1] + modL[1]);
                    headTimingSnapshot.h3TimeMs = clampDelay(baseL[2] + modL[2]);
                    headTimingSnapshot.head2RatioSetting = HEAD2_RATIO;
                    headTimingSnapshot.head3RatioSetting = HEAD3_RATIO;
                    headTimingSnapshot.wowFlutterMs = WF_L;
                    headTimingSnapshot.headRatioPreset = HeadRatioPreset;
                    headTimingSnapshot.stereoMode = StereoMode;
                    headTimingSnapshot.tempoSyncEnabled = TempoSyncEnabled;
                    headTimingSnapshot.sampleIndex = headTimingSampleCounter;
                    headTimingSnapshot.valid = true;
                }
            }
#endif

            // ================================================================
            // SINGLE BUFFER MULTI-TAP READ (shared-tape architecture)
            // ================================================================
            // Read from ONE buffer at 3 different tap points (head positions)
            // This eliminates comb filtering from 3 independent feedback loops
            // ================================================================
            float ynL_PH[3] = {0.0f, 0.0f, 0.0f};
            float ynR_PH[3] = {0.0f, 0.0f, 0.0f};

            const float* targetBase = syncCrossfadeTargetMs;
            const float prevBase[3] = {syncCrossfadePrevMs[0], syncCrossfadePrevMs[1], syncCrossfadePrevMs[2]};

            for (int head = 0; head < 3; ++head)
            {
                if (TempoSyncEnabled && syncCrossfadeActive[head])
                {
                    const float fade = lookupCosineFade(syncCrossfadeProgress[head]);
                    float oldDelayL = clampDelay(prevBase[head] +
                                                 scaleDelayModulationForHead(WF_L, prevBase[head], delayTimeSmoothedL));
                    float newDelayL = clampDelay(
                        targetBase[head] + scaleDelayModulationForHead(WF_L, targetBase[head], delayTimeSmoothedL));
                    float oldDelayR = clampDelay(prevBase[head] +
                                                 scaleDelayModulationForHead(WF_R, prevBase[head], delayTimeSmoothedR));
                    float newDelayR = clampDelay(
                        targetBase[head] + scaleDelayModulationForHead(WF_R, targetBase[head], delayTimeSmoothedR));
                    // Read from SINGLE buffer at different tap points
                    const float oldL = TapeBuffer[0]->BufferRead(oldDelayL, true);
                    const float newL = TapeBuffer[0]->BufferRead(newDelayL, true);
                    const float oldR = TapeBuffer[1]->BufferRead(oldDelayR, true);
                    const float newR = TapeBuffer[1]->BufferRead(newDelayR, true);
                    ynL_PH[head] = oldL + (newL - oldL) * fade;
                    ynR_PH[head] = oldR + (newR - oldR) * fade;

                    advanceSyncCrossfade(head);
                }
                else
                {
                    // Read from SINGLE buffer at different tap points (head positions)
                    float delayL = clampDelay(baseL[head] + modL[head]);
                    float delayR = clampDelay(baseR[head] + modR[head]);
                    ynL_PH[head] = TapeBuffer[0]->BufferRead(delayL, true);
                    ynR_PH[head] = TapeBuffer[1]->BufferRead(delayR, true);
                }
            }

            float ynL_PH1 = ynL_PH[0];
            float ynL_PH2 = ynL_PH[1];
            float ynL_PH3 = ynL_PH[2];
            float ynR_PH1 = ynR_PH[0];
            float ynR_PH2 = ynR_PH[1];
            float ynR_PH3 = ynR_PH[2];

            // ================================================================
            // Design: SPLICE/DROPOUT APPLIED TO HEAD READS
            // ================================================================
            // In hardware, the splice is ON THE TAPE - so it modulates what the
            // playback heads read. This affects BOTH output AND feedback.
            // Uses canonical motor state for splice timing.
            // ================================================================
            float loopTimeSec = (LOOP_TIME_RATIO * delayTimeSmoothedMotor) / 1000.0f;
            float phaseIncrement = (loopTimeSec > 0.0f) ? (1.0f / (loopTimeSec * SampleRate)) : 0.0f;
            tapePhase += phaseIncrement;
            if (tapePhase >= 1.0f)
                tapePhase -= 1.0f;

            // Calculate splice envelope (gain dip at splice point)
            // Note: spliceWowBurst already calculated and added to WF above
            const float spliceEnv = calculateSpliceGain();

            // Apply splice + dropout to head reads (affects feedback AND output)
            float tapeGain = spliceEnv * dropoutGain;
            ynL_PH1 *= tapeGain;
            ynL_PH2 *= tapeGain;
            ynL_PH3 *= tapeGain;
            ynR_PH1 *= tapeGain;
            ynR_PH2 *= tapeGain;
            ynR_PH3 *= tapeGain;
            applyOutputNoiseTapeGain(sample, tapeGain);

            // Smooth intensity changes to prevent clicks.
            IntensitySmoothed = intensitySmoothCoeff * IntensitySmoothed + (1.0f - intensitySmoothCoeff) * Intensity;

            // Apply dub switch
            float gatedInputL = inputL * EchoSendGain;
            float gatedInputR = inputR * EchoSendGain;

            // Detect activity causally at the record point. Looking ahead over
            // the host callback makes an onset's seed angle depend on callback
            // size and lets muted raw input steer the loop. A short fixed hold
            // bridges zero crossings; the first active frame after it expires
            // snaps immediately, while active-program changes retain the 5 ms
            // angle slew.
            constexpr double kIdentityEnergyRatio = 1.0e-6;
            constexpr double kBalancedEnergyRatio = 1.0e-4;
            const double gatedEnergyL =
                std::isfinite(gatedInputL) ? static_cast<double>(gatedInputL) * static_cast<double>(gatedInputL) : 0.0;
            const double gatedEnergyR =
                std::isfinite(gatedInputR) ? static_cast<double>(gatedInputR) * static_cast<double>(gatedInputR) : 0.0;
            const double dominantEnergy = std::max(gatedEnergyL, gatedEnergyR);
            const bool frameHasActiveInput = dominantEnergy > PING_PONG_ACTIVE_FRAME_ENERGY;
            const bool detectorWasActive = pingPongActivitySamplesRemaining > 0;

            if (frameHasActiveInput)
            {
                pingPongActivitySamplesRemaining = pingPongActivityReleaseSamples;

                const double quietEnergy = std::min(gatedEnergyL, gatedEnergyR);
                const double energyRatio = quietEnergy / dominantEnergy;
                float balancedAmount = 0.0f;
                if (energyRatio >= kBalancedEnergyRatio)
                {
                    balancedAmount = 1.0f;
                }
                else if (energyRatio > kIdentityEnergyRatio)
                {
                    const double logRatio = std::log10(energyRatio);
                    const float linearAmount =
                        static_cast<float>((logRatio - std::log10(kIdentityEnergyRatio)) /
                                           (std::log10(kBalancedEnergyRatio) - std::log10(kIdentityEnergyRatio)));
                    balancedAmount = linearAmount * linearAmount * (3.0f - 2.0f * linearAmount);
                }

                const float targetSeedAngle = PING_PONG_BALANCED_ROTATION * balancedAmount;
                if (!pingPongSeedAngleInitialized || !detectorWasActive)
                {
                    pingPongSeedAngle = targetSeedAngle;
                    pingPongSeedTargetAngle = targetSeedAngle;
                    pingPongSeedAngleStep = 0.0f;
                    pingPongSeedRampRemaining = 0;
                    pingPongSeedAngleInitialized = true;
                    seedCos = std::cos(pingPongSeedAngle);
                    seedSin = std::sin(pingPongSeedAngle);
                    seedStepCos = 1.0f;
                    seedStepSin = 0.0f;
                    pingPongSeedPhasorRenormalizeRemaining = PING_PONG_PHASOR_RENORMALIZE_INTERVAL;
                }
                else if (std::abs(targetSeedAngle - pingPongSeedTargetAngle) > 1.0e-6f)
                {
                    pingPongSeedTargetAngle = targetSeedAngle;
                    pingPongSeedRampRemaining = pingPongSeedRampSamples;
                    pingPongSeedAngleStep =
                        (pingPongSeedTargetAngle - pingPongSeedAngle) / static_cast<float>(pingPongSeedRampRemaining);
                    seedStepCos = std::cos(pingPongSeedAngleStep);
                    seedStepSin = std::sin(pingPongSeedAngleStep);
                }
            }
            else if (pingPongActivitySamplesRemaining > 0)
            {
                --pingPongActivitySamplesRemaining;
            }

            // ================================================================
            // Design: SINGLE RECORD POINT MODEL
            // ================================================================
            // In this single-record-head model, the post-tape-gain H1 stereo tap
            // owns regeneration and passes through one record chain per channel.
            // Audible head enables affect presentation and capture, not regeneration.
            // ================================================================

            // Smooth head enable changes (clickless toggles)
            if (!headEnableInitialized)
            {
                headEnableGain[0] = headEnableTarget[0];
                headEnableGain[1] = headEnableTarget[1];
                headEnableGain[2] = headEnableTarget[2];
                headEnableInitialized = true;
            }
            for (int h = 0; h < 3; ++h)
            {
                if (headEnableGain[h] < headEnableTarget[h])
                    headEnableGain[h] = std::min(headEnableTarget[h], headEnableGain[h] + headEnableStep);
                else if (headEnableGain[h] > headEnableTarget[h])
                    headEnableGain[h] = std::max(headEnableTarget[h], headEnableGain[h] - headEnableStep);
            }
            const float h1Gain = headEnableGain[0];
            const float h2Gain = headEnableGain[1];
            const float h3Gain = headEnableGain[2];
            const std::array<float, 3> audibleHeadGains{h1Gain, h2Gain, h3Gain};
            const std::array<std::array<float, 2>, 3> audibleHeadSamples{
                {{{ynL_PH1, ynR_PH1}}, {{ynL_PH2, ynR_PH2}}, {{ynL_PH3, ynR_PH3}}}};

            const float masterFeedbackL = ynL_PH1;
            const float masterFeedbackR = ynR_PH1;
            float effectiveIntensity = IntensitySmoothed;
            if (killActive)
                effectiveIntensity *= killEnvelope;

            // ================================================================
            // SINGLE BUFFER ARCHITECTURE (shared-tape pattern)
            // ================================================================
            // ONE buffer per channel, ONE write per sample.
            // Output sums enabled heads while regeneration always uses H1.
            // This eliminates comb filtering from multiple independent loops
            // ================================================================

            // ===== FEEDBACK GAIN COMPENSATION =====
            // Apply compensation before the record write so the protection controls
            // the loop itself, not only the already-generated output.
            const float peakL = std::abs(masterFeedbackL);
            const float peakR = std::abs(masterFeedbackR);
            feedbackAvgLevel[0] = feedbackAvgCoeff * feedbackAvgLevel[0] + (1.0f - feedbackAvgCoeff) * peakL;
            feedbackAvgLevel[1] = feedbackAvgCoeff * feedbackAvgLevel[1] + (1.0f - feedbackAvgCoeff) * peakR;
            const float gainCompL =
                feedbackCompensationEnabled ? getFeedbackGainCompensation(feedbackAvgLevel[0]) : 1.0f;
            const float gainCompR =
                feedbackCompensationEnabled ? getFeedbackGainCompensation(feedbackAvgLevel[1]) : 1.0f;

            float feedbackL = masterFeedbackL * gainCompL;
            float feedbackR = masterFeedbackR * gainCompR;

            // ================================================================
            // FEEDBACK PATH FILTERING - KEY FOR PROGRESSIVE DARKENING
            // ================================================================
            // Apply LPF and HPF to feedback BEFORE combining with input.
            // This means each repeat passes through the filter, getting progressively
            // darker. First echo = bright, subsequent echoes = darker.
            // This is how real tape echo works - tape itself acts as a low-pass filter.
            // ================================================================
            float filteredFeedbackL =
                FeedbackLPfilters[0]->ProcessSample(FeedbackHPfilters[0]->ProcessSample(feedbackL));
            float filteredFeedbackR =
                FeedbackLPfilters[1]->ProcessSample(FeedbackHPfilters[1]->ProcessSample(feedbackR));

            // Calculate one pair of candidate record writes. The Ping-Pong
            // orthonormal rotation preserves stereo energy at every blend
            // angle. Balanced mono rotates to the left seed, opposite polarity
            // rotates to the right seed, and hard-panned input remains on its
            // source side so subsequent cross-feedback generations alternate.
            const float seedL = gatedInputL * seedCos + gatedInputR * seedSin;
            const float seedR = -gatedInputL * seedSin + gatedInputR * seedCos;
            const float normalWriteL = gatedInputL + effectiveIntensity * filteredFeedbackL;
            const float normalWriteR = gatedInputR + effectiveIntensity * filteredFeedbackR;
            const float pingPongWriteL = seedL + effectiveIntensity * filteredFeedbackR;
            const float pingPongWriteR = seedR + effectiveIntensity * filteredFeedbackL;

            const float currentWriteL = StereoMode == 1 ? pingPongWriteL : normalWriteL;
            const float currentWriteR = StereoMode == 1 ? pingPongWriteR : normalWriteR;
            float preSatWriteL = currentWriteL;
            float preSatWriteR = currentWriteR;
            if (crossfadeActive)
            {
                const float fade = lookupCosineFade(crossfadeProgress);
                const float previousWriteL = PreviousStereoMode == 1 ? pingPongWriteL : normalWriteL;
                const float previousWriteR = PreviousStereoMode == 1 ? pingPongWriteR : normalWriteR;
                preSatWriteL = previousWriteL + (currentWriteL - previousWriteL) * fade;
                preSatWriteR = previousWriteR + (currentWriteR - previousWriteR) * fade;
            }

            const float loopExcitation = std::max(std::abs(preSatWriteL), std::abs(preSatWriteR));
            tapeNoiseActivityForEnvelope = loopExcitation;
            updateSelfOscHeat(loopExcitation);
            const float loopNoiseGate = updateLoopNoiseGate(loopExcitation);
            const float recordInL = processRecordFilters(preSatWriteL, 0);
            const float recordInR = processRecordFilters(preSatWriteR, 1);
            // Apply the record stage once per stereo frame so parameter
            // smoothing is channel-count invariant.
            float writeL = recordInL;
            float writeR = recordInR;
            recordPointSaturationStereo(writeL, writeR);
            const float loopNoise = NoiseProcessor.getLoopNoise() * loopNoiseGate;

            // Apply DC blocker after saturation (removes offset from asymmetric saturation)
            writeL = processDCBlock(writeL, 0);
            writeR = processDCBlock(writeR, 1);
            writeL += loopNoise;
            writeR += loopNoise;

            // Apply kill envelope to buffer writes
            if (killActive)
            {
                writeL *= killEnvelope;
                writeR *= killEnvelope;
            }

            // Write ONCE to single tape buffer per channel
            TapeBuffer[0]->BufferWrite(writeL);
            TapeBuffer[1]->BufferWrite(writeR);

            if (crossfadeActive)
            {
                crossfadeProgress += crossfadeStep;
                if (crossfadeProgress >= 1.0f)
                {
                    crossfadeProgress = 1.0f;
                    crossfadeActive = false;
                    PreviousStereoMode = StereoMode;
                }
            }

            // ===== CANONICAL AUDIBLE HEAD BUSES =====
            // Treat each stereo head independently through the same linear
            // playback chain, store that exact signal for Independent mode,
            // then sum it for the normal Linked echo bus.
            const float audibleKillGain = killActive ? killEnvelope : 1.0f;
            float outputL = 0.0f;
            float outputR = 0.0f;
            for (int head = 0; head < 3; ++head)
            {
                const float treatedL = processAudibleHeadSample(audibleHeadSamples[static_cast<size_t>(head)][0] *
                                                                    audibleHeadGains[static_cast<size_t>(head)],
                                                                head, 0, audibleKillGain, DelayEnabledGain);
                const float treatedR = processAudibleHeadSample(audibleHeadSamples[static_cast<size_t>(head)][1] *
                                                                    audibleHeadGains[static_cast<size_t>(head)],
                                                                head, 1, audibleKillGain, DelayEnabledGain);

                if (perHeadCaptureEnabled)
                {
                    storePerHeadCaptureSample(head, 0, sample, treatedL);
                    storePerHeadCaptureSample(head, 1, sample, treatedR);
                }

                outputL += treatedL;
                outputR += treatedR;
            }

            buffer[0][static_cast<size_t>(sample)] = outputL;
            buffer[1][static_cast<size_t>(sample)] = outputR;

            if (killActive)
            {
                killEnvelope -= killStep;
                if (killEnvelope <= 0.0f)
                {
                    killEnvelope = 0.0f;
                    killActive = false;
                    clearBuffers();
                }
            }

            if (pingPongSeedRampRemaining > 0)
            {
                const float nextSeedCos = seedCos * seedStepCos - seedSin * seedStepSin;
                seedSin = seedSin * seedStepCos + seedCos * seedStepSin;
                seedCos = nextSeedCos;
                pingPongSeedAngle += pingPongSeedAngleStep;
                --pingPongSeedRampRemaining;
                if (pingPongSeedRampRemaining == 0)
                {
                    pingPongSeedAngle = pingPongSeedTargetAngle;
                    pingPongSeedAngleStep = 0.0f;
                    seedCos = std::cos(pingPongSeedTargetAngle);
                    seedSin = std::sin(pingPongSeedTargetAngle);
                    pingPongSeedPhasorRenormalizeRemaining = PING_PONG_PHASOR_RENORMALIZE_INTERVAL;
                }
                else if (--pingPongSeedPhasorRenormalizeRemaining <= 0)
                {
                    // A continuously moving stereo ratio can restart the ramp
                    // indefinitely, so endpoint canonicalization alone is not
                    // enough. Renormalize on a persisted sample cadence to
                    // preserve both unit energy and callback partition parity.
                    const float phasorNormSquared = seedCos * seedCos + seedSin * seedSin;
                    if (std::isfinite(phasorNormSquared) && phasorNormSquared > 1.0e-12f)
                    {
                        const float inverseNorm = 1.0f / std::sqrt(phasorNormSquared);
                        seedCos *= inverseNorm;
                        seedSin *= inverseNorm;
                    }
                    else
                    {
                        seedCos = std::cos(pingPongSeedAngle);
                        seedSin = std::sin(pingPongSeedAngle);
                    }
                    pingPongSeedPhasorRenormalizeRemaining = PING_PONG_PHASOR_RENORMALIZE_INTERVAL;
                }
            }
        }
        pingPongSeedCos = seedCos;
        pingPongSeedSin = seedSin;
    }
    else
    {
        // MONO MODE: Single channel processing
        for (int sample = 0; sample < blockSize; sample++)
        {
            float xn = buffer[0][static_cast<size_t>(sample)];

            DelayEnabledGain = rampToward(DelayEnabledGain, DelayEnabledTarget, delayEnableStep);
            EchoSendGain = rampToward(EchoSendGain, EchoSendTarget, echoSendStep);

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
            if (captureHeadTiming)
                ++headTimingSampleCounter;
#endif

            // Motor state and noise advance before read/write decisions.
            updateTempoSyncSmoothedDelays();
            float delayTimeSmoothed = processTransportDelayMs(0);
            const float noiseEnvelopeInput = std::max(std::abs(xn), tapeNoiseActivityForEnvelope);
            NoiseProcessor.processSample(noiseEnvelopeInput);
            if (sample < noiseWriteLimit)
            {
                outputNoisePerSample[static_cast<size_t>(sample)] = NoiseProcessor.getOutputNoise();
                if (!outputNoisePerChannel.empty())
                    outputNoisePerChannel[0][static_cast<size_t>(sample)] = NoiseProcessor.getOutputNoiseForChannel(0);
            }
            float dropoutGain = NoiseProcessor.getDropoutGain();

            float WF = WFProcessors[0]->processSample() + calculateSpliceWowBurst();
            WF = shapeDelayModulation(WF, 0);

            // TEMPO SYNC: Per-head subdivision delays (user-configurable)
            // Classic mode: Uses head ratios (1:1.901:2.751)

            // Validate delay times - clamp to safe range (same as stereo path)
            auto clampDelayMono = [](float ms)
            {
                if (std::isnan(ms) || std::isinf(ms))
                    return 70.0f;
                return std::max(1.0f, std::min(ms, 6000.0f));
            };

            float base[3] = {0.0f, 0.0f, 0.0f};
            if (TempoSyncEnabled)
            {
                base[0] = Head1SyncSmoothed;
                base[1] = Head2SyncSmoothed;
                base[2] = Head3SyncSmoothed;
            }
            else
            {
                base[0] = delayTimeSmoothed;
                base[1] = delayTimeSmoothed * HEAD2_RATIO;
                base[2] = delayTimeSmoothed * HEAD3_RATIO;
            }

            float mod[3] = {0.0f, 0.0f, 0.0f};
            for (int head = 0; head < 3; ++head)
                mod[head] = scaleDelayModulationForHead(WF, base[head], delayTimeSmoothed);

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
            if (captureHeadTiming && !headTimingCaptured)
            {
                const float inputPeak = std::abs(xn);
                if (inputPeak > 0.01f)
                {
                    headTimingCaptured = true;
                    headTimingSnapshot.delayTimeTargetMs = getTransportTargetDelayMs();
                    headTimingSnapshot.delayTimeSmoothedMs = delayTimeSmoothed;
                    // Head times on the non-crossfade path.
                    headTimingSnapshot.h1TimeMs = clampDelayMono(base[0] + mod[0]);
                    headTimingSnapshot.h2TimeMs = clampDelayMono(base[1] + mod[1]);
                    headTimingSnapshot.h3TimeMs = clampDelayMono(base[2] + mod[2]);
                    headTimingSnapshot.head2RatioSetting = HEAD2_RATIO;
                    headTimingSnapshot.head3RatioSetting = HEAD3_RATIO;
                    headTimingSnapshot.wowFlutterMs = WF;
                    headTimingSnapshot.headRatioPreset = HeadRatioPreset;
                    headTimingSnapshot.stereoMode = StereoMode;
                    headTimingSnapshot.tempoSyncEnabled = TempoSyncEnabled;
                    headTimingSnapshot.sampleIndex = headTimingSampleCounter;
                    headTimingSnapshot.valid = true;
                }
            }
#endif

            // SINGLE BUFFER MULTI-TAP READ (mono path)
            float ynPH[3] = {0.0f, 0.0f, 0.0f};
            const float* targetBase = syncCrossfadeTargetMs;
            const float prevBase[3] = {syncCrossfadePrevMs[0], syncCrossfadePrevMs[1], syncCrossfadePrevMs[2]};

            for (int head = 0; head < 3; ++head)
            {
                if (TempoSyncEnabled && syncCrossfadeActive[head])
                {
                    const float fade = lookupCosineFade(syncCrossfadeProgress[head]);
                    float oldDelay = clampDelayMono(prevBase[head] +
                                                    scaleDelayModulationForHead(WF, prevBase[head], delayTimeSmoothed));
                    float newDelay = clampDelayMono(
                        targetBase[head] + scaleDelayModulationForHead(WF, targetBase[head], delayTimeSmoothed));
                    // Read from SINGLE buffer at different tap points
                    const float oldSample = TapeBuffer[0]->BufferRead(oldDelay, true);
                    const float newSample = TapeBuffer[0]->BufferRead(newDelay, true);
                    ynPH[head] = oldSample + (newSample - oldSample) * fade;

                    advanceSyncCrossfade(head);
                }
                else
                {
                    float delay = clampDelayMono(base[head] + mod[head]);
                    ynPH[head] = TapeBuffer[0]->BufferRead(delay, true);
                }
            }

            float ynPH1 = ynPH[0];
            float ynPH2 = ynPH[1];
            float ynPH3 = ynPH[2];

            // Design: SPLICE/DROPOUT APPLIED TO HEAD READS (mono path)
            // In hardware, splice is on the tape - affects both feedback and output
            float loopTimeSec = (LOOP_TIME_RATIO * delayTimeSmoothed) / 1000.0f;
            float phaseIncrement = (loopTimeSec > 0.0f) ? (1.0f / (loopTimeSec * SampleRate)) : 0.0f;
            tapePhase += phaseIncrement;
            if (tapePhase >= 1.0f)
                tapePhase -= 1.0f;

            const float spliceEnv = calculateSpliceGain();

            // Apply splice + dropout to head reads
            float tapeGain = spliceEnv * dropoutGain;
            ynPH1 *= tapeGain;
            ynPH2 *= tapeGain;
            ynPH3 *= tapeGain;
            applyOutputNoiseTapeGain(sample, tapeGain);

            // Smooth intensity changes to prevent clicks.
            // Note: Smoothing happens in stereo path too, this ensures mono path stays in sync
            IntensitySmoothed = intensitySmoothCoeff * IntensitySmoothed + (1.0f - intensitySmoothCoeff) * Intensity;

            float gatedInput = xn * EchoSendGain;

            // Design: SINGLE RECORD POINT MODEL (mono path)
            // Regenerate from the post-tape-gain H1 master tap, then process
            // and write that signal once independently of audible head enables.
            if (!headEnableInitialized)
            {
                headEnableGain[0] = headEnableTarget[0];
                headEnableGain[1] = headEnableTarget[1];
                headEnableGain[2] = headEnableTarget[2];
                headEnableInitialized = true;
            }
            for (int h = 0; h < 3; ++h)
            {
                if (headEnableGain[h] < headEnableTarget[h])
                    headEnableGain[h] = std::min(headEnableTarget[h], headEnableGain[h] + headEnableStep);
                else if (headEnableGain[h] > headEnableTarget[h])
                    headEnableGain[h] = std::max(headEnableTarget[h], headEnableGain[h] - headEnableStep);
            }
            const float h1Gain = headEnableGain[0];
            const float h2Gain = headEnableGain[1];
            const float h3Gain = headEnableGain[2];
            const float masterFeedback = ynPH1;
            const std::array<float, 3> audibleHeadGains{h1Gain, h2Gain, h3Gain};
            const std::array<float, 3> audibleHeadSamples{ynPH1, ynPH2, ynPH3};
            float effectiveIntensity = IntensitySmoothed;
            if (killActive)
                effectiveIntensity *= killEnvelope;

            // ===== FEEDBACK GAIN COMPENSATION (mono path) =====
            const float peakMono = std::abs(masterFeedback);
            feedbackAvgLevel[0] = feedbackAvgCoeff * feedbackAvgLevel[0] + (1.0f - feedbackAvgCoeff) * peakMono;
            const float gainCompMono =
                feedbackCompensationEnabled ? getFeedbackGainCompensation(feedbackAvgLevel[0]) : 1.0f;

            // SINGLE BUFFER ARCHITECTURE (mono path)
            float feedback = masterFeedback * gainCompMono;

            // Apply feedback filtering for progressive darkening (mono path)
            float filteredFeedback = FeedbackLPfilters[0]->ProcessSample(FeedbackHPfilters[0]->ProcessSample(feedback));

            // Design: apply the bounded stateful record stage after feedback filtering.
            // NOTE: Using FILTERED feedback for progressive darkening
            const float preSatWrite = gatedInput + effectiveIntensity * filteredFeedback;
            const float loopNoiseGate = updateLoopNoiseGate(std::abs(preSatWrite));
            tapeNoiseActivityForEnvelope = std::abs(preSatWrite);
            updateSelfOscHeat(std::abs(preSatWrite));
            const float recordIn = processRecordFilters(preSatWrite, 0);
            float writeSample = recordPointSaturation(recordIn, 0);

            // Apply DC blocker after saturation (mono path)
            writeSample = processDCBlock(writeSample, 0);

            float loopNoise = NoiseProcessor.getLoopNoise() * loopNoiseGate;
            writeSample += loopNoise;

            if (killActive)
            {
                writeSample *= killEnvelope;
            }

            // Write ONCE to single tape buffer
            TapeBuffer[0]->BufferWrite(writeSample);

            const float audibleKillGain = killActive ? killEnvelope : 1.0f;
            float output = 0.0f;
            for (int head = 0; head < 3; ++head)
            {
                const float treated = processAudibleHeadSample(audibleHeadSamples[static_cast<size_t>(head)] *
                                                                   audibleHeadGains[static_cast<size_t>(head)],
                                                               head, 0, audibleKillGain, DelayEnabledGain);
                if (perHeadCaptureEnabled)
                    storePerHeadCaptureSample(head, 0, sample, treated);
                output += treated;
            }

            buffer[0][static_cast<size_t>(sample)] = output;

            if (killActive)
            {
                killEnvelope -= killStep;
                if (killEnvelope <= 0.0f)
                {
                    killEnvelope = 0.0f;
                    killActive = false;
                    clearBuffers();
                }
            }
        }
    }

    // Hand the self-oscillation heat envelope to the GUI once per block.
    publishSelfOscHeat();
}

void TapeDelayProcessor::triggerKill()
{
    // Start the kill fade - envelope goes from 1.0 to 0.0 over KILL_FADE_TIME_SEC.
    // SampleRate is zero until Reset() runs; without the guard the step becomes
    // inf/NaN and the first fade multiplies the buffer by a non-finite envelope.
    killActive = true;
    killEnvelope = 1.0f;
    killStep = (SampleRate > 0.0f) ? (1.0f / (KILL_FADE_TIME_SEC * SampleRate)) : 1.0f;
}

void TapeDelayProcessor::cancelKill()
{
    // Cancel any ongoing kill fade when power is turned back on
    killActive = false;
    killEnvelope = 0.0f;
}

void TapeDelayProcessor::clearBuffers()
{
    // Clear all circular buffers (the "tape").
    // NumChannels is a plain parameter mirror and can legitimately exceed the
    // number of prepared channels (default 2 before Reset() has ever run, or
    // after a debug split-path call). Bound the loop by what actually exists so
    // clearBuffers() before prepare cannot index an empty vector.
    const int preparedChannels = static_cast<int>(TapeBuffer.size());
    const int clearChannels = std::min(NumChannels, preparedChannels);
    for (int channel = 0; channel < clearChannels; channel++)
    {
        if (TapeBuffer[static_cast<size_t>(channel)])
            TapeBuffer[static_cast<size_t>(channel)]->ClearBuffer();

        // Reset filters in feedback path
        if (channel < static_cast<int>(FeedbackHPfilters.size()) && FeedbackHPfilters[static_cast<size_t>(channel)])
            FeedbackHPfilters[static_cast<size_t>(channel)]->Reset(SampleRate);
        if (channel < static_cast<int>(FeedbackLPfilters.size()) && FeedbackLPfilters[static_cast<size_t>(channel)])
            FeedbackLPfilters[static_cast<size_t>(channel)]->Reset(SampleRate);
    }
    for (size_t head = 0; head < 3; ++head)
    {
        for (size_t channel = 0; channel < 2; ++channel)
        {
            perHeadPlaybackHPFilters[head][channel].Reset(SampleRate);
            perHeadPlaybackLPFilters[head][channel].Reset(SampleRate);
            perHeadBumpFilters[head][channel].Reset(SampleRate);
        }
    }

    // Reset kill state
    killActive = false;
    killEnvelope = 0.0f;
    feedbackAvgLevel[0] = 0.0f;
    feedbackAvgLevel[1] = 0.0f;
    wfModLPState[0] = wfModLPState[1] = 0.0f;
    wfModSlewState[0] = wfModSlewState[1] = 0.0f;
    recordLPF_state[0] = recordLPF_state[1] = 0.0f;
    recordLPF2_state[0] = recordLPF2_state[1] = 0.0f;
    recordHPF_state[0] = recordHPF_state[1] = 0.0f;
    recordDCBlock_state[0] = recordDCBlock_state[1] = 0.0f;
    recordSaturator.reset();
    tapeNoiseActivityForEnvelope = 0.0f;
    resetLoopNoiseGate();
    pingPongActivitySamplesRemaining = 0;
    selfOscHeatEnv = 0.0f;
    selfOscHeatPublished.store(0.0f, std::memory_order_relaxed);
}

float TapeDelayProcessor::processAudibleHeadSample(float sample, int headIdx, int channelIdx, float killGain,
                                                   float delayGain)
{
    if (headIdx < 0 || headIdx >= 3 || channelIdx < 0 || channelIdx >= 2)
        return 0.0f;

    const float bumped =
        perHeadBumpFilters[static_cast<size_t>(headIdx)][static_cast<size_t>(channelIdx)].ProcessSample(sample);
    sample += (bumped - sample) * headBumpAmount;
    sample *= killGain;
    sample =
        perHeadPlaybackHPFilters[static_cast<size_t>(headIdx)][static_cast<size_t>(channelIdx)].ProcessSample(sample);
    sample =
        perHeadPlaybackLPFilters[static_cast<size_t>(headIdx)][static_cast<size_t>(channelIdx)].ProcessSample(sample);

    return sample * delayGain;
}

void TapeDelayProcessor::storePerHeadCaptureSample(int headIdx, int channelIdx, int sampleIdx, float value)
{
    if (headIdx < 0 || headIdx >= 3 || channelIdx < 0 || channelIdx >= 2 || sampleIdx < 0)
        return;

    auto& channelBuffer = perHeadCaptureBuffers[static_cast<size_t>(headIdx)][static_cast<size_t>(channelIdx)];
    if (static_cast<size_t>(sampleIdx) >= channelBuffer.size())
        return;

    channelBuffer[static_cast<size_t>(sampleIdx)] = value;
}

void TapeDelayProcessor::enablePerHeadCapture(bool enabled)
{
    perHeadCaptureEnabled = enabled;
    // Zero buffers when disabled to avoid stale data leaking
    if (!enabled)
    {
        for (size_t h = 0; h < 3; ++h)
        {
            for (size_t ch = 0; ch < 2; ++ch)
                std::fill(perHeadCaptureBuffers[h][ch].begin(), perHeadCaptureBuffers[h][ch].end(), 0.0f);
        }
    }
}

bool TapeDelayProcessor::isPerHeadCaptureEnabled() const
{
    return perHeadCaptureEnabled;
}

const float* TapeDelayProcessor::getHeadOutput(int headIdx) const
{
    return getHeadOutput(headIdx, 0);
}

const float* TapeDelayProcessor::getHeadOutput(int headIdx, int channelIdx) const
{
    if (headIdx < 0 || headIdx >= 3)
        return nullptr;
    if (channelIdx < 0 || channelIdx >= NumChannels || channelIdx >= 2)
        return nullptr;
    if (perHeadCaptureBuffers[static_cast<size_t>(headIdx)][static_cast<size_t>(channelIdx)].empty())
        return nullptr;
    return perHeadCaptureBuffers[static_cast<size_t>(headIdx)][static_cast<size_t>(channelIdx)].data();
}
