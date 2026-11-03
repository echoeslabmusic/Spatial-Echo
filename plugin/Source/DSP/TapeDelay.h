// TapeDelayProcessor: Multi-head tape echo with configurable head ratios and feedback.
// Realtime-safety: No allocation/locks in process()

#pragma once

#include "dependencies/CircularBuffer.h"
#include "dependencies/wowflutter.h"
#include "dependencies/OnePole.h"
#include "dependencies/RbjBiquad.h"
#include "TapeNoiseProcessor.h"
#include "TapeRecordSaturator.h"
#include "HeadRatioConstants.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>

class TapeDelayProcessor
{
public:
    // ---- Playback-head tone contour -------------------------------------
    // Public so the rendered-response tests can derive their expected bands
    // from the shipping values instead of keeping a private copy that rots
    // silently the next time the voicing is recalibrated.
    //
    // Speed contour: the playback low-pass tracks transport speed between
    // these two cutoffs (longest delay = slowest tape = darkest).
    static constexpr float PLAYBACK_SLOW_CUTOFF_HZ = 14000.0f;
    static constexpr float PLAYBACK_FAST_CUTOFF_HZ = 18500.0f;
    // Playback low end. Q is Butterworth ON PURPOSE. A Q of 0.25 splits the
    // RBJ pole pair to 0.268*fc and 3.73*fc, so a nominal 40 Hz high-pass
    // behaves as a first-order ~149 Hz high-pass: the rendered echo measures
    // -15.0 dB at 50 Hz, -6.2 dB at 100 Hz and -2.2 dB at 200 Hz with a -3 dB
    // corner at 167 Hz.
    // Tape reproduce chains are not built like that: the NAB/IEC 3180 us
    // low-frequency time constant is a 50 Hz corner and machine specs quote
    // 30-40 Hz at the bottom end.
    static constexpr float PLAYBACK_HP_CUTOFF_HZ = 40.0f;
    static constexpr float PLAYBACK_HP_Q = 0.70710678f;

    TapeDelayProcessor();
    ~TapeDelayProcessor();

    void Reset(float sampleRate, int numChannels, int maxBlockSize = 16384);
    // The 317/460 ms defaults are display/reference values only. The live
    // classic path derives head timing from HEAD1_BASE_DELAY_MS and its ratios.
    // The named constants below ARE the UpdateParameters default arguments, so
    // the static assertions pin the actual defaults to the classic ratios
    // (mutating a default alone fails compilation).
    static constexpr float HEAD1_REFERENCE_DEFAULT_MS = 167.0f;
    static constexpr float HEAD2_REFERENCE_DEFAULT_MS = 317.0f;
    static constexpr float HEAD3_REFERENCE_DEFAULT_MS = 460.0f;
    // Exact pins: ANY mutation of a reference default fails compilation
    // (the ratio asserts below allow ±1 ms and would miss a 1 ms drift).
    static_assert(HEAD1_REFERENCE_DEFAULT_MS > 166.999f && HEAD1_REFERENCE_DEFAULT_MS < 167.001f,
                  "Head 1 reference default is pinned at 167 ms");
    static_assert(HEAD2_REFERENCE_DEFAULT_MS > 316.999f && HEAD2_REFERENCE_DEFAULT_MS < 317.001f,
                  "Head 2 reference default is pinned at 317 ms");
    static_assert(HEAD3_REFERENCE_DEFAULT_MS > 459.999f && HEAD3_REFERENCE_DEFAULT_MS < 460.001f,
                  "Head 3 reference default is pinned at 460 ms");
    static_assert(HeadRatios::HEAD1_BASE_DELAY_MS > HEAD1_REFERENCE_DEFAULT_MS - 0.001f &&
                      HeadRatios::HEAD1_BASE_DELAY_MS < HEAD1_REFERENCE_DEFAULT_MS + 0.001f,
                  "Head 1 reference default must equal the classic base delay");
    static_assert(HeadRatios::HEAD1_BASE_DELAY_MS * HeadRatios::CLASSIC_RATIO_2 > HEAD2_REFERENCE_DEFAULT_MS - 1.0f &&
                      HeadRatios::HEAD1_BASE_DELAY_MS * HeadRatios::CLASSIC_RATIO_2 < HEAD2_REFERENCE_DEFAULT_MS + 1.0f,
                  "Head 2 reference default must stay within 1 ms of the classic ratio");
    static_assert(HeadRatios::HEAD1_BASE_DELAY_MS * HeadRatios::CLASSIC_RATIO_3 > HEAD3_REFERENCE_DEFAULT_MS - 1.0f &&
                      HeadRatios::HEAD1_BASE_DELAY_MS * HeadRatios::CLASSIC_RATIO_3 < HEAD3_REFERENCE_DEFAULT_MS + 1.0f,
                  "Head 3 reference default must stay within 1 ms of the classic ratio");
    void UpdateParameters(float intensity, float delaytime, int playheadEnabled[], int delayEnabled,
                          float echoSend = 1.0f, bool noiseEnabled = false, float noiseAmount = 0.12f,
                          float tapeCondition = 0.08f, float wowFlutterDepth = 1.0f, float wowRate = 1.0f,
                          float flutterRate = 1.0f, bool tempoSyncEnabled = false, float head1SyncDelayMs = 0.0f,
                          float head2SyncDelayMs = 0.0f, float head3SyncDelayMs = 0.0f, int stereoMode = 0,
                          int headRatioPreset = 0, bool feedbackCompensationEnabled = true,
                          float presetAsperity = -1.0f, float presetDropout = -1.0f, float presetScrapeScale = 1.0f,
                          float presetLpfScale = 1.0f, float feedbackUserIntensity = -1.0f);

    // Set head timing ratios based on preset index
    // 0 = Classic (1:1.901:2.751), 1 = Idealized (1:2:3), 2 = Golden (1:1.618:2.618), 3 = Linear (1:1.5:2)
    void setHeadRatioPreset(int preset);
    // Generate the current callback's independent tank-noise bed before any
    // routing branch consumes it. ProcessBuffer consumes this preparation once;
    // direct TapeDelay callers get the same preparation automatically.
    void prepareReverbNoiseBuffer(const std::vector<std::vector<float>>& input, int blockSize);
    void ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize);

    // Kill/Clear buffer - triggers smooth fade-out then clears delay buffers
    void triggerKill();

    // Cancel ongoing kill fade (when power turned back on)
    void cancelKill();

    // Immediate clear of all delay buffers and loop filter state (no fade).
    // Used by the kill fade on completion and by the auto-panic hard flush,
    // where fading is pointless (NaN times any gain is still NaN).
    void clearBuffers();

    // Noise access for output injection
    float getOutputNoise() const { return NoiseProcessor.getOutputNoise(); }

    // Self-oscillation "heat": how hard the record-point tape saturator is being
    // driven, as a fast-attack / slow-release envelope of the pre-saturation record
    // drive (|input + feedback|). ~1.0 is the soft-clip knee where saturation first
    // becomes audible; higher values mean progressively grosser distortion. The slow
    // release makes a sustaining feedback loop hold the reading high while one-shot
    // input transients decay away. Published once per block; the editor reads it to
    // colour the PANIC light green -> amber -> red. Lock-free, GUI-thread safe.
    float getSelfOscHeat() const noexcept { return selfOscHeatPublished.load(std::memory_order_relaxed); }
    // Per-sample output noise generated during the last ProcessBuffer call.
    const std::vector<float>& getOutputNoiseBuffer() const { return outputNoisePerSample; }
    const std::vector<float>& getOutputNoiseBufferForChannel(int channel) const
    {
        if (outputNoisePerChannel.empty())
            return outputNoisePerSample;

        const size_t index =
            static_cast<size_t>(std::clamp(channel, 0, static_cast<int>(outputNoisePerChannel.size()) - 1));
        return outputNoisePerChannel[index];
    }

    // Per-sample reverb-injection noise generated during the last ProcessBuffer
    // call, one buffer per channel. Same contract as
    // getOutputNoiseBufferForChannel(): valid for the most recent
    // ProcessBuffer(), indexed [0, blockSize), channel index clamped into
    // range, never empty once Reset() has run.
    //
    // This exists because a single block-held scalar cannot represent a noise
    // bed: adding one value to every sample of a block produces a staircase at
    // the block rate (~93 Hz at 512 samples / 48 kHz) plus its harmonics, and
    // the value is a block stale on top of that whenever the reverb is rendered
    // before the delay. Unlike the output bed, this is deliberately NOT scaled
    // by the tape splice/dropout gain: the reverb bed models tank noise, not
    // something recorded onto the tape.
    const std::vector<float>& getReverbNoiseBufferForChannel(int channel) const
    {
        if (reverbNoisePerChannel.empty())
            return outputNoisePerSample;

        const size_t index =
            static_cast<size_t>(std::clamp(channel, 0, static_cast<int>(reverbNoisePerChannel.size()) - 1));
        return reverbNoisePerChannel[index];
    }

    // Check if delay is enabled (for reverb-only mode routing)
    // Keep enabled while fade-out is in progress to avoid clicks.
    bool isDelayEnabled() const { return (DelayEnabledTarget > 0.0f) || (DelayEnabledGain > 1.0e-4f); }

    // First-party record-stage character. The saturator is owned here because
    // its state belongs to the delay feedback write path.
    void setTapeCharacter(float drive, float saturation, float bias) noexcept
    {
        recordSaturator.setCharacter(drive, saturation, bias);
    }

    void setTapeRecordMakeupGainDb(float makeupGainDb) noexcept { recordSaturator.setMakeupGainDb(makeupGainDb); }

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    // Live head timing recorded by ProcessBuffer for the head-timing tests
    // (see TapeDelayTestAccess::enableHeadTimingCapture).
    struct HeadTimingSnapshot
    {
        float delayTimeTargetMs = 0.0f;
        float delayTimeSmoothedMs = 0.0f;
        float h1TimeMs = 0.0f;
        float h2TimeMs = 0.0f;
        float h3TimeMs = 0.0f;
        float head2RatioSetting = 0.0f;
        float head3RatioSetting = 0.0f;
        float wowFlutterMs = 0.0f;
        int headRatioPreset = 0;
        int stereoMode = 0;
        bool tempoSyncEnabled = false;
        int64_t sampleIndex = 0;
        bool valid = false;
    };

#endif

    // Per-head capture (for Independent mode spatial routing)
    void enablePerHeadCapture(bool enabled);
    bool isPerHeadCaptureEnabled() const;
    const float* getHeadOutput(int headIdx) const;
    const float* getHeadOutput(int headIdx, int channelIdx) const;

private:
    friend struct TapeDelayTestAccess;

    // DSP objects
    // SINGLE BUFFER per channel (shared-tape architecture)
    // Multiple read taps simulate 3 playback heads on ONE tape
    // This eliminates comb filtering from 3 independent feedback loops
    std::vector<std::unique_ptr<CircularBuffer>> TapeBuffer;
    std::vector<std::unique_ptr<OnePole>> BallisticsFilters;
    std::vector<std::unique_ptr<WowFlutterSignalModel>> WFProcessors;

    // Feedback path filters - applied PER REPEAT for progressive darkening
    // This is the key to authentic tape echo: each repeat gets darker
    std::vector<std::unique_ptr<RbjBiquad>> FeedbackLPfilters;
    std::vector<std::unique_ptr<RbjBiquad>> FeedbackHPfilters;
    static constexpr float PLAYBACK_CONDITION_DEPTH = 0.20f;
    static constexpr float FEEDBACK_BASE_CUTOFF_HZ = 9500.0f;
    static constexpr float FEEDBACK_CONDITION_DEPTH = 0.25f;
    static constexpr float FEEDBACK_MIN_CUTOFF_HZ = 6000.0f;
    static constexpr float FEEDBACK_MAX_CUTOFF_HZ = 12000.0f;
    float playbackFilterCutoff = PLAYBACK_SLOW_CUTOFF_HZ;
    float feedbackFilterCutoff = FEEDBACK_BASE_CUTOFF_HZ;

    // Head bump resonance - adds low-frequency "thump" characteristic of tape machines.
    // Each audible head owns independent linear playback-filter state so the
    // treated head buses sum to the same signal used by Linked mode.
    float headBumpAmount = 0.12f;  // Blend amount (0 = off, 0.15 = subtle, 0.3 = pronounced)

    // Tape noise processor (hiss)
    TapeNoiseProcessor NoiseProcessor;
    // Separate state makes tank noise causal even when reverb feeds the tape.
    // Sharing the tape's output stream would need a previous-callback read and
    // make its timing depend on the host block size.
    TapeNoiseProcessor ReverbNoiseProcessor;
    echoes::TapeRecordSaturator recordSaturator;
    std::vector<float> outputNoisePerSample;
    std::vector<std::vector<float>> outputNoisePerChannel;
    std::vector<std::vector<float>> reverbNoisePerChannel;
    int preparedReverbNoiseSamples = 0;
    float tapeNoiseActivityForEnvelope = 0.0f;

    // Per-head capture buffers (for Independent mode spatial routing)
    // Captures the fully treated stereo contribution of each head.
    std::array<std::array<std::vector<float>, 2>, 3> perHeadCaptureBuffers;
    bool perHeadCaptureEnabled = false;
    std::array<std::array<RbjBiquad, 2>, 3> perHeadPlaybackHPFilters;
    std::array<std::array<RbjBiquad, 2>, 3> perHeadPlaybackLPFilters;
    std::array<std::array<RbjBiquad, 2>, 3> perHeadBumpFilters;

    // Plugin variables
    int NumChannels = 2;

    float Intensity = 0.0f;
    float IntensitySmoothed = 0.0f;  // Smoothed intensity to prevent clicks on abrupt changes
    // τ=4.16 ms at every sample rate, deliberately faster than the 15-60 ms
    // ramp family so the feedback knob keeps its existing responsive feel.
    // Reset() derives this coefficient per sample rate; 48 kHz is pinned to
    // exactly 0.995f so existing 48 kHz renders remain bit-identical.
    static constexpr float INTENSITY_SMOOTH_TAU_SECONDS = 0.00416f;
    float intensitySmoothCoeff = 0.995f;
    float DelayTime = 0.0f;
    float SampleRate = 0.0f;
    bool delayTimePrimed = false;

    int DelayEnabled = 0;
    float DelayEnabledGain = 1.0f;
    float DelayEnabledTarget = 1.0f;
    bool DelayEnabledInitialized = false;
    static constexpr float DELAY_ENABLE_FADE_TIME_SEC = 0.06f;
    float delayEnableStep = 0.0f;

    // Echo/Normal "dub" switch: 1.0 = ECHO (input feeds delay), 0.0 = NORMAL (input muted, feedback continues)
    float EchoSend = 1.0f;
    float EchoSendGain = 1.0f;
    float EchoSendTarget = 1.0f;
    bool EchoSendInitialized = false;
    static constexpr float ECHO_SEND_FADE_TIME_SEC = 0.015f;
    float echoSendStep = 0.0f;

    // Feedback gain compensation
    // Keep this in the "safety limiter" role so normal oscillation is not damped too early.
    float feedbackAvgLevel[2] = {0.0f, 0.0f};
    // The compensation engages late on purpose so normal self-oscillation is
    // not damped. SelfOscBloomProfile at 48 kHz with the threshold at 0.95: no
    // compensation pumping, no Auto-Panic events, 20.3 dB watchdog margin.
    static constexpr float FEEDBACK_COMP_THRESHOLD = 0.95f;
    // Averaging time constant for the feedback safety limiter. A fixed
    // per-sample coefficient of 0.9995 would make the averaging window shrink
    // with sample rate (41.7 ms at 48 kHz, 10.4 ms at 192 kHz) and the limiter
    // react four times faster on a 192 kHz session. The coefficient is derived
    // from this time constant instead; 48 kHz resolves to exactly 0.9995f.
    static constexpr float FEEDBACK_AVG_TAU_SECONDS = 0.0416563f;
    float feedbackAvgCoeff = 0.9995f;
    bool feedbackCompensationEnabled = true;

    // One-pole coefficient for a target time constant, with 48 kHz pinned to
    // the literal 0.9995f so 48 kHz renders stay bit-identical.
    static float onePoleCoeffForTau(float tauSeconds, float sampleRate, float pinnedAt48k)
    {
        if (sampleRate == 48000.0f)
            return pinnedAt48k;
        if (!(sampleRate > 0.0f) || !(tauSeconds > 0.0f))
            return pinnedAt48k;
        return std::exp(-1.0f / (tauSeconds * sampleRate));
    }

    inline float getFeedbackGainCompensation(float avgLevel) const
    {
        return (avgLevel > FEEDBACK_COMP_THRESHOLD) ? (FEEDBACK_COMP_THRESHOLD / avgLevel) : 1.0f;
    }

    // Gate loop-noise seeding so dormant loops do not resurrect on unrelated parameter moves.
    // Thresholds are in linear full-scale.
    static constexpr float LOOP_NOISE_GATE_OPEN_THRESHOLD = 0.0018f;   // ~-55 dBFS
    static constexpr float LOOP_NOISE_GATE_CLOSE_THRESHOLD = 0.0010f;  // ~-60 dBFS
    float loopNoiseGateEnv = 0.0f;
    float loopNoiseGateGain = 0.0f;
    bool loopNoiseGateOpen = false;
    float loopNoiseGateAttackCoeff = 0.0f;
    float loopNoiseGateReleaseCoeff = 0.0f;

    inline void resetLoopNoiseGate()
    {
        loopNoiseGateEnv = 0.0f;
        loopNoiseGateGain = 0.0f;
        loopNoiseGateOpen = false;
    }

    inline float updateLoopNoiseGate(float excitation)
    {
        const float absExcitation = std::abs(excitation);
        const float envCoeff =
            (absExcitation > loopNoiseGateEnv) ? loopNoiseGateAttackCoeff : loopNoiseGateReleaseCoeff;
        loopNoiseGateEnv += (absExcitation - loopNoiseGateEnv) * envCoeff;

        if (loopNoiseGateOpen)
            loopNoiseGateOpen = (loopNoiseGateEnv > LOOP_NOISE_GATE_CLOSE_THRESHOLD);
        else
            loopNoiseGateOpen = (loopNoiseGateEnv >= LOOP_NOISE_GATE_OPEN_THRESHOLD);

        const float gateTarget = loopNoiseGateOpen ? 1.0f : 0.0f;
        const float gateCoeff = loopNoiseGateOpen ? loopNoiseGateAttackCoeff : loopNoiseGateReleaseCoeff;
        loopNoiseGateGain += (gateTarget - loopNoiseGateGain) * gateCoeff;
        return loopNoiseGateGain;
    }

    // ===== Self-oscillation heat metering (drives the PANIC light) =====
    // Envelope of the pre-saturation record drive: fast attack catches the loop
    // building up, slow release holds the reading so a sustaining self-oscillation
    // stays lit while transient input decays. Published to an atomic once per block
    // so the GUI can read it without a lock. Reset by Reset()/clearBuffers().
    float selfOscHeatEnv = 0.0f;
    float selfOscHeatAttackCoeff = 0.0f;
    float selfOscHeatReleaseCoeff = 0.0f;
    std::atomic<float> selfOscHeatPublished{0.0f};

    inline void updateSelfOscHeat(float drive) noexcept
    {
        const float x = std::abs(drive);
        const float coeff = (x > selfOscHeatEnv) ? selfOscHeatAttackCoeff : selfOscHeatReleaseCoeff;
        selfOscHeatEnv += (x - selfOscHeatEnv) * coeff;
    }

    inline void publishSelfOscHeat() noexcept { selfOscHeatPublished.store(selfOscHeatEnv, std::memory_order_relaxed); }

    // Tempo sync state - per-head subdivision delays
    bool TempoSyncEnabled = false;
    float Head1SyncDelayMs = 0.0f;  // Base subdivision (e.g., 1/4 note)
    float Head2SyncDelayMs = 0.0f;  // User-selected subdivision (e.g., 1/8 note)
    float Head3SyncDelayMs = 0.0f;  // User-selected subdivision (e.g., 1/16 note)

    // Smoothed per-head delays (prevents clicks on subdivision changes)
    float Head1SyncSmoothed = 0.0f;
    float Head2SyncSmoothed = 0.0f;
    float Head3SyncSmoothed = 0.0f;

    // Tempo sync crossfade state (clickless subdivision changes)
    bool syncCrossfadeActive[3] = {false, false, false};
    float syncCrossfadeProgress[3] = {0.0f, 0.0f, 0.0f};
    float syncCrossfadePrevMs[3] = {0.0f, 0.0f, 0.0f};
    float syncCrossfadeTargetMs[3] = {0.0f, 0.0f, 0.0f};
    static constexpr float SYNC_CROSSFADE_TIME_SEC = 0.015f;     // 15ms crossfade
    static constexpr float SYNC_CROSSFADE_THRESHOLD_MS = 10.0f;  // crossfade audible sync-delay changes
    float syncCrossfadeStep = 0.0f;
    // Glide applied to sub-threshold tempo-sync delay changes: a 41.7 ms time
    // constant, which is a 0.9995 per-sample coefficient at 48 kHz.
    static constexpr float TEMPO_SYNC_GLIDE_TAU_SECONDS = 0.0416563f;
    float tempoSyncGlideCoeff = 0.9995f;

    // Stereo mode: 0=Normal, 1=Ping-Pong
    int StereoMode = 0;
    int PreviousStereoMode = 0;  // For crossfade detection

    // Ping-Pong input seeding rotates continuously between identity for
    // hard-panned material and a 45-degree stereo rotation for balanced input.
    // Activity is detected causally from the actual gated record input. A new
    // onset after the fixed release interval snaps to the content-derived
    // angle before its first sample; continuously active changes slew over a
    // fixed time independent of the host callback size.
    static constexpr float PING_PONG_BALANCED_ROTATION = 0.7853981633974483f;
    static constexpr float PING_PONG_SEED_SLEW_TIME_SEC = 0.005f;
    static constexpr float PING_PONG_ACTIVITY_RELEASE_TIME_SEC = 0.005f;
    static constexpr double PING_PONG_ACTIVE_FRAME_ENERGY = 1.0e-12;
    static constexpr int PING_PONG_PHASOR_RENORMALIZE_INTERVAL = 64;
    float pingPongSeedAngle = PING_PONG_BALANCED_ROTATION;
    float pingPongSeedTargetAngle = PING_PONG_BALANCED_ROTATION;
    float pingPongSeedAngleStep = 0.0f;
    float pingPongSeedCos = 0.7071067811865476f;
    float pingPongSeedSin = 0.7071067811865476f;
    int pingPongSeedPhasorRenormalizeRemaining = PING_PONG_PHASOR_RENORMALIZE_INTERVAL;
    int pingPongSeedRampSamples = 1;
    int pingPongSeedRampRemaining = 0;
    int pingPongActivityReleaseSamples = 1;
    int pingPongActivitySamplesRemaining = 0;
    bool pingPongSeedAngleInitialized = false;

    // Mode switching crossfade state (prevents clicks)
    bool crossfadeActive = false;
    float crossfadeProgress = 0.0f;                      // 0.0 = old mode, 1.0 = new mode
    static constexpr float CROSSFADE_TIME_SEC = 0.015f;  // 15ms crossfade
    float crossfadeStep = 0.0f;                          // Per-sample increment

    // Precomputed envelope LUTs to avoid per-sample trig in hot loops.
    static constexpr int kEnvTableSize = 512;
    std::array<float, kEnvTableSize> cosineFadeLut{};
    std::array<float, kEnvTableSize> sineLut{};
    bool lutReady = false;

    void initEnvelopeLuts()
    {
        if (lutReady)
            return;
        const float sizeMinusOne = static_cast<float>(kEnvTableSize - 1);
        for (int i = 0; i < kEnvTableSize; ++i)
        {
            const float t = static_cast<float>(i) / sizeMinusOne;
            cosineFadeLut[(size_t)i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * 0.5f * t);
            sineLut[(size_t)i] = std::sin(juce::MathConstants<float>::twoPi * t);
        }
        lutReady = true;
    }

    float lookupCosineFade(float t) const
    {
        if (t <= 0.0f)
            return 0.0f;
        if (t >= 1.0f)
            return 1.0f;
        const float idx = t * static_cast<float>(kEnvTableSize - 1);
        const int i0 = static_cast<int>(idx);
        const int i1 = std::min(i0 + 1, kEnvTableSize - 1);
        const float frac = idx - static_cast<float>(i0);
        return cosineFadeLut[(size_t)i0] + (cosineFadeLut[(size_t)i1] - cosineFadeLut[(size_t)i0]) * frac;
    }

    float lookupSine(float t) const
    {
        if (t <= 0.0f)
            return 0.0f;
        if (t >= 1.0f)
            return 0.0f;
        const float idx = t * static_cast<float>(kEnvTableSize - 1);
        const int i0 = static_cast<int>(idx);
        const int i1 = std::min(i0 + 1, kEnvTableSize - 1);
        const float frac = idx - static_cast<float>(i0);
        return sineLut[(size_t)i0] + (sineLut[(size_t)i1] - sineLut[(size_t)i0]) * frac;
    }

    float calculateSpliceGain() const
    {
        if (splicePhaseWidth <= 0.0f || tapePhase >= splicePhaseWidth)
            return 1.0f;

        // Enter and leave the splice at unity with zero endpoint slope. A
        // one-way fade jumped from maximum attenuation back to unity at the
        // exit, injecting a click into playback and every feedback generation.
        // Folding the cosine keeps the same width, depth and mean attenuation.
        const float phase = tapePhase / splicePhaseWidth;
        const float dipPhase = 2.0f * std::min(phase, 1.0f - phase);
        return 1.0f - spliceIntensity * lookupCosineFade(dipPhase);
    }

    void applyOutputNoiseTapeGain(int sample, float tapeGain)
    {
        const size_t index = static_cast<size_t>(sample);
        if (index < outputNoisePerSample.size())
            outputNoisePerSample[index] *= tapeGain;

        for (auto& channelNoise : outputNoisePerChannel)
        {
            if (index < channelNoise.size())
                channelNoise[index] *= tapeGain;
        }
    }

    float calculateSpliceWowBurst() const
    {
        const float spliceWowPhaseWidth = splicePhaseWidth * 2.5f;
        if (spliceWowPhaseWidth <= 0.0f || spliceWowAmount <= 0.0f)
            return 0.0f;

        const float spliceWowPhaseOffset = splicePhaseWidth * 1.5f;
        float wowPhasePos = tapePhase - spliceWowPhaseOffset;
        if (wowPhasePos < 0.0f)
            wowPhasePos += 1.0f;

        if (wowPhasePos >= spliceWowPhaseWidth)
            return 0.0f;

        return spliceWowAmount * lookupSine(wowPhasePos / spliceWowPhaseWidth);
    }

    // Playhead States
    int PH1enabled = 0;
    int PH2enabled = 0;
    int PH3enabled = 0;

    // Head enable smoothing (clickless head toggles)
    float headEnableGain[3] = {0.0f, 0.0f, 0.0f};
    float headEnableTarget[3] = {0.0f, 0.0f, 0.0f};
    bool headEnableInitialized = false;
    static constexpr float HEAD_ENABLE_FADE_TIME_SEC = 0.015f;
    float headEnableStep = 0.0f;

    // Head timing ratios - user-configurable with presets (values from HeadRatioConstants.h)
    float HEAD2_RATIO = HeadRatios::CLASSIC_RATIO_2;
    float HEAD3_RATIO = HeadRatios::CLASSIC_RATIO_3;
    int HeadRatioPreset = 0;

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    bool captureHeadTiming = false;
    bool headTimingCaptured = false;
    HeadTimingSnapshot headTimingSnapshot{};
    int64_t headTimingSampleCounter = 0;
#endif

    // Linear mapping function
    float map(float x, float in_min, float in_max, float out_min, float out_max)
    {
        return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
    }

    static float rampToward(float current, float target, float step)
    {
        if (current < target)
            return std::min(target, current + step);
        if (current > target)
            return std::max(target, current - step);
        return current;
    }

    static float clampTransportDelay(float ms, float fallback = 175.0f)
    {
        if (!std::isfinite(ms))
            ms = fallback;
        return std::clamp(ms, 1.0f, 6000.0f);
    }

    static float scaleDelayModulationForHead(float modulationMs, float headBaseMs, float referenceBaseMs)
    {
        if (!std::isfinite(modulationMs) || !std::isfinite(headBaseMs) || !std::isfinite(referenceBaseMs) ||
            referenceBaseMs <= 1.0e-3f)
            return modulationMs;

        const float scale = std::clamp(headBaseMs / referenceBaseMs, 0.25f, 8.0f);
        return modulationMs * scale;
    }

    void updateTempoSyncSmoothedDelays()
    {
        if (!TempoSyncEnabled)
            return;

        if (Head1SyncSmoothed < 1.0f && Head1SyncDelayMs > 10.0f)
        {
            Head1SyncSmoothed = Head1SyncDelayMs;
            Head2SyncSmoothed = Head2SyncDelayMs;
            Head3SyncSmoothed = Head3SyncDelayMs;
            return;
        }

        // Tempo-transition glide. The coefficient is derived from
        // TEMPO_SYNC_GLIDE_TAU_SECONDS in Reset() so the glide time holds at
        // every sample rate; 48 kHz resolves to exactly 0.9995f.
        const float alpha = tempoSyncGlideCoeff;
        Head1SyncSmoothed = alpha * Head1SyncSmoothed + (1.0f - alpha) * Head1SyncDelayMs;
        Head2SyncSmoothed = alpha * Head2SyncSmoothed + (1.0f - alpha) * Head2SyncDelayMs;
        Head3SyncSmoothed = alpha * Head3SyncSmoothed + (1.0f - alpha) * Head3SyncDelayMs;
    }

    void advanceSyncCrossfade(int head)
    {
        syncCrossfadeProgress[head] += syncCrossfadeStep;
        if (syncCrossfadeProgress[head] < 1.0f)
            return;

        const float completedDelay = syncCrossfadeTargetMs[head];
        const float requestedDelay = head == 0 ? Head1SyncDelayMs : head == 1 ? Head2SyncDelayMs : Head3SyncDelayMs;
        if (head == 0)
            Head1SyncSmoothed = completedDelay;
        if (head == 1)
            Head2SyncSmoothed = completedDelay;
        if (head == 2)
            Head3SyncSmoothed = completedDelay;
        syncCrossfadePrevMs[head] = completedDelay;

        // A new request during a fade must not replace either audible read or
        // restart its weight. Finish that mixture, then take the latest request
        // from its now fully audible destination. Small changes retain the glide.
        const bool hasQueuedTransition =
            requestedDelay > 1.0f && std::abs(requestedDelay - completedDelay) >= SYNC_CROSSFADE_THRESHOLD_MS;
        syncCrossfadeActive[head] = hasQueuedTransition;
        syncCrossfadeProgress[head] = hasQueuedTransition ? 0.0f : 1.0f;
        if (hasQueuedTransition)
            syncCrossfadeTargetMs[head] = requestedDelay;
    }

    float getMasterTransportDelayMs(bool smoothed) const
    {
        if (!TempoSyncEnabled)
            return clampTransportDelay(DelayTime);

        const float head1 = smoothed && Head1SyncSmoothed > 1.0f ? Head1SyncSmoothed : Head1SyncDelayMs;
        if (!std::isfinite(head1) || head1 <= 1.0f)
            return clampTransportDelay(DelayTime);
        return clampTransportDelay(head1);
    }

    float getTransportTargetDelayMs() const { return getMasterTransportDelayMs(false); }

    float processTransportDelayMs(int channel)
    {
        const float targetDelay = getMasterTransportDelayMs(true);
        if (channel >= 0 && channel < static_cast<int>(BallisticsFilters.size()) && BallisticsFilters[(size_t)channel])
            return clampTransportDelay(BallisticsFilters[(size_t)channel]->processAsymmetric(targetDelay), targetDelay);
        return targetDelay;
    }

    // Bandlimit and slew-limit delay modulation to avoid high-rate
    // time-varying delay artifacts when scrape components are active.
    inline float shapeDelayModulation(float modulationMs, int channel)
    {
        if (channel < 0 || channel >= 2)
            return modulationMs;

        if (!std::isfinite(modulationMs))
            modulationMs = 0.0f;

        wfModLPState[channel] += wfModLPCoeff * (modulationMs - wfModLPState[channel]);
        const float target = wfModLPState[channel];
        const float delta = std::clamp(target - wfModSlewState[channel], -wfModMaxDeltaMs, wfModMaxDeltaMs);
        wfModSlewState[channel] += delta;
        return wfModSlewState[channel];
    }

    // ========================================================================
    // Single record point model
    // ========================================================================
    // In a single-record-head tape echo, there is ONE record head. The signal flow is:
    //   Post-tape-gain H1 master tap -> Feedback Filter -> Record Saturation -> Tape
    // H1/H2/H3 still read the same tape for audible presentation, but their
    // enable states and presentation gains do not alter regeneration.
    //
    // Filter state is per-channel only (not per-head) because there's one record path.
    // Playback owns speed/condition tone. This record filter stays fixed and
    // wide so it conditions the nonlinearity without adding another time law.
    // ========================================================================

    // Record chain filters (one per channel - models the single record head)
    float recordLPF_state[2] = {0.0f, 0.0f};      // HF rolloff (tape gap loss, stage 1)
    float recordLPF2_state[2] = {0.0f, 0.0f};     // HF rolloff (tape gap loss, stage 2)
    float recordHPF_state[2] = {0.0f, 0.0f};      // LF rolloff (prevents DC/mud buildup)
    float recordDCBlock_state[2] = {0.0f, 0.0f};  // Post-saturation DC blocking
    static constexpr float RECORD_ANTI_ALIAS_CUTOFF_HZ = 18000.0f;
    static constexpr float RECORD_ANTI_ALIAS_NYQUIST_FRACTION = 0.40f;
    float recordFilterCutoff = RECORD_ANTI_ALIAS_CUTOFF_HZ;
    float recordLPF_coeff = 0.85f;
    float recordHPF_coeff = 0.995f;      // ~60Hz
    float recordDCBlock_coeff = 0.995f;  // ~30Hz DC blocker

    // Delay-modulation anti-alias conditioning.
    float wfModLPState[2] = {0.0f, 0.0f};
    float wfModSlewState[2] = {0.0f, 0.0f};
    float wfModLPCoeff = 0.0f;
    float wfModMaxDeltaMs = 0.0f;

    float recordChainSampleRate = 0.0f;

    // Pre-saturation filtering (applied to combined feedback mix)
    inline float processRecordFilters(float x, int channel)
    {
        // HPF first (prevents LF buildup)
        float hp = x - recordHPF_state[channel];
        recordHPF_state[channel] += (1.0f - recordHPF_coeff) * hp;

        // Two cascaded one-pole LPFs (12 dB/oct equivalent) for stronger
        // anti-alias conditioning before record-point nonlinearity.
        recordLPF_state[channel] = (1.0f - recordLPF_coeff) * hp + recordLPF_coeff * recordLPF_state[channel];
        recordLPF2_state[channel] =
            (1.0f - recordLPF_coeff) * recordLPF_state[channel] + recordLPF_coeff * recordLPF2_state[channel];
        return recordLPF2_state[channel];
    }

    // Post-saturation DC blocking (removes offset from asymmetric saturation)
    inline float processDCBlock(float x, int channel)
    {
        float y = x - recordDCBlock_state[channel];
        recordDCBlock_state[channel] += (1.0f - recordDCBlock_coeff) * y;
        return y;
    }

    // Design: the feedback write owns one bounded, stateful record stage.
    // Stereo frames advance shared parameter smoothing exactly once while each
    // channel retains independent magnetic-memory state.
    inline float recordPointSaturation(float x, int channel) noexcept
    {
        return recordSaturator.processMono(x, channel);
    }

    inline void recordPointSaturationStereo(float& left, float& right) noexcept
    {
        recordSaturator.processStereoFrame(left, right);
    }

    float processAudibleHeadSample(float sample, int headIdx, int channelIdx, float killGain, float delayGain);
    void storePerHeadCaptureSample(int headIdx, int channelIdx, int sampleIdx, float value);

    // Kill/Clear state - smooth fade to avoid clicks
    bool killActive = false;
    float killEnvelope = 0.0f;
    float killStep = 0.0f;                              // Per-sample decrement
    static constexpr float KILL_FADE_TIME_SEC = 0.02f;  // 20ms fade

    // Design: Virtual tape loop with splice artifact once per revolution.
    // The splice causes a subtle dropout/thump once per revolution.
    //
    // PHASE ACCUMULATOR MODEL: a continuous phase (0..1) advances each sample by
    // an increment proportional to the SMOOTHED motor speed, so splice timing
    // ramps with the motor ballistics rather than stepping abruptly as a fixed
    // loop length would when parameters change.
    float tapePhase = 0.0f;          // Current position in tape loop (0..1)
    float splicePhaseWidth = 0.01f;  // Width of splice as fraction of loop

    // Design: Loop timing calibration constant
    // Formula: T_loop = K * t_head1, where K = physicalTapeLength / distanceToHead1
    // This is an internal calibration constant - not user-facing.
    // K is a design constant chosen by ear rather than measured from hardware.
    static constexpr float LOOP_TIME_RATIO = 17.14f;
    static constexpr float REFERENCE_DELAY_MS = 175.0f;  // Design: Slowest delay baseline
    float spliceIntensity = 0.02f;                       // Exponentially scaled with tape condition (2-30%)
    float spliceWowAmount = 0.0f;                        // Pitch deviation when splice passes capstan (ms)
};
