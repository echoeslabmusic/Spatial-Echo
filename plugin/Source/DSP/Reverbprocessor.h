// ReverbProcessor: Multi-engine reverb with spring and cloud models.
// Realtime-safety: No allocation/locks in process()

#pragma once

#include "dependencies/WaveguideSpringReverb.h"
#include "dependencies/CloudSeedCore/CloudSeedReverb.h"
#include <memory>
#include <cmath>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

/**
 * ReverbMacroParams - the user-facing reverb schema.
 * Seven continuous macros + engine choice; ReverbProcessor derives all
 * engine-native parameters internally. Defaults reproduce the reference
 * default sound.
 */
struct ReverbMacroParams
{
    float size = 0.7085f;    // 0..1 - tank/room size; also drives early/late balance
    float decay = 0.6802f;   // 0..1 - T60
    float tone = 0.5f;       // 0=dark .. 1=bright (damping + wet low/high cut)
    float mod = 0.08f;       // 0..1 - modulation depth; rates fixed per engine
    float character = 0.5f;  // Spring: boing/tension; Cloud: bloom/density
    float preDelay = 0.0f;   // 0..1 → 0..200 ms
    float width = 0.4f;      // 0=mono .. 1=wide
    int engine = 0;          // 0=Spring, 1=Cloud
};

/**
 * ReverbProcessor - multi-engine reverb
 *
 * Engines:
 *   0 = Spring (WaveguideSpringReverbNetwork mono tank + width stereoizer)
 *   1 = Cloud  (CloudSeedCore algorithmic reverb, always stereo)
 *
 * Design: the spring is always a single shared mono tank (real tanks are
 * mono); stereo comes from a post-tank modulated-allpass stereoizer whose
 * side level tracks the Width macro (mono-compatible: L+R stays 2×tank).
 * There is no per-channel dual-tank mode.
 */
class ReverbProcessor
{
public:
    ReverbProcessor();
    ~ReverbProcessor();

    void Reset(float sampleRateHz, int numChannels, int maxBlockSize = 512);
    // Rare audio-thread safety operation. This destroys both live tanks and
    // cancels any switch/drain so non-finite state cannot survive the block.
    void hardClearNow();
    void UpdateParameters(int reverbEnabled, float reverbAmount, const ReverbMacroParams& macros,
                          bool noiseEnabled = false, float noiseAmount = 0.12f);
    void ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize);

private:
    friend struct ReverbProcessorTestAccess;

    // Opposing size/decay macro sweeps measure 10.4-14.5 dB p-p output flutter
    // (ReverbMacroSmoothingFlutterProbe), and unifying these two constants to
    // 150 ms measures 16.3 dB, so the 200/150 ms split is NOT the flutter
    // mechanism: inherent structural time-variance dominates. Reducing that
    // flutter needs a structural-change crossfade, not a different smoothing time.
    static constexpr float kStructuralSmoothingMs = 200.0f;
    static constexpr float kControlSmoothingMs = 150.0f;

    // Engine-native parameters derived from the macros. An engine's entry is
    // replaced by its derived target on the first UpdateParameters call that
    // selects that engine (engineParamsInitialized), so the zero defaults here
    // are never rendered.
    struct SmoothedReverbParams
    {
        float size = 0.0f;
        float decay = 0.0f;
        float damping = 0.0f;
        float diffusion = 0.0f;
        float modRate = 0.0f;
        float modDepth = 0.0f;
        float springBalance = 0.0f;
        float scatterAmount = 0.0f;
        float preDelay = 0.0f;
        float earlyLateMix = 0.0f;
        float presentationGain = 0.0f;
        float lowCutHz = 0.0f;
        float highCutHz = 0.0f;
        float width = 0.0f;
    };

    // DSP objects - Spring engine (single mono tank)
    std::unique_ptr<WaveguideSpringReverbNetwork> ClassicReverb;

    // DSP objects - Cloud engine
    std::unique_ptr<CloudSeedReverb> cloudReverb;
    int ReverbEngine = 0;        // Target engine: 0=Spring, 1=Cloud
    int activeReverbEngine = 0;  // Engine at unity outside a transition
    int transitionFromEngine = 0;
    int transitionToEngine = 0;

    // Modulated-allpass decorrelator stage. Used as the Spring engine's width
    // stereoizer: side = (decorL(tank) − decorR(tank))/2 × widthGain, added
    // symmetrically so L+R always folds back to the pure mono tank.
    //
    // TripleSpringReverb's ModulatedAllpass is the same structure, but it runs
    // its LFO as a recursive rotation and flushes z1 before the output instead
    // of after, so the two are not sample-identical and stay separate.
    struct StereoizerAllpass
    {
        // 4096 samples covers the longest 13 ms stage plus modulation at the
        // supported 192 kHz ceiling. Runtime clamps still keep every read
        // inside the array if a host reports a higher or invalid rate.
        static constexpr int kMaxDelay = 4096;
        float buf[kMaxDelay] = {};
        int writeIdx = 0;
        float z1 = 0.0f;  // allpass interpolation state
        float phase = 0.0f;
        int baseDelay = 0;
        float modDepthSamples = 0.0f;
        float phaseInc = 0.0f;
        float feedback = 0.5f;

        void prepare(float sr, float delayMs, float modDepthMs, float rateHz, float fb)
        {
            constexpr float maximumDelay = static_cast<float>(kMaxDelay - 3);
            modDepthSamples = std::clamp(modDepthMs * sr * 0.001f, 0.0f, maximumDelay - 1.0f);
            const float maximumBaseDelay = std::max(1.0f, maximumDelay - modDepthSamples);
            baseDelay = std::clamp(static_cast<int>(std::lround(delayMs * sr * 0.001f)), 1,
                                   static_cast<int>(std::floor(maximumBaseDelay)));
            phaseInc = rateHz / sr;
            feedback = fb;
            reset();
        }

        void reset()
        {
            std::memset(buf, 0, sizeof(buf));
            writeIdx = 0;
            z1 = 0.0f;
        }

        float process(float input)
        {
            phase += phaseInc;
            if (phase >= 1.0f)
                phase -= 1.0f;
            float mod = std::sin(phase * juce::MathConstants<float>::twoPi);
            float totalDelay = (float)baseDelay + modDepthSamples * mod;
            // The allpass reader biases its fractional part into [0.5, 1.5)
            // and so reads one sample further back than totalDelay.
            totalDelay = std::clamp(totalDelay, 2.0f, static_cast<float>(kMaxDelay - 3));
            int intDel = (int)totalDelay;
            float frac = totalDelay - (float)intDel;
            if (frac < 0.5f)
            {
                intDel -= 1;
                frac += 1.0f;
            }

            int idxA = writeIdx - intDel;
            if (idxA < 0)
                idxA += kMaxDelay;
            int idxB = idxA - 1;
            if (idxB < 0)
                idxB += kMaxDelay;

            // First-order-allpass fractional delay (flat magnitude, exact DC
            // group delay). See InterpolatedDelayLine::allpassRead for why the
            // fractional part is biased into [0.5, 1.5): the alternative
            // `z1 = buf[idxB] + frac * (buf[idxA] - z1)` runs the delay
            // backwards inside each integer bin and steps two whole samples at
            // every bin crossing, which at these modulation depths is ~100
            // audible steps per second per stage.
            const float eta = (1.0f - frac) / (1.0f + frac);
            z1 = eta * buf[idxA] + buf[idxB] - eta * z1;
            float bufOut = z1;

            float inVal = input + bufOut * feedback;
            buf[writeIdx] = inVal;
            float output = bufOut - inVal * feedback;

            writeIdx = (writeIdx + 1) % kMaxDelay;
            // Denormal prevention
            if (std::abs(z1) < 1e-15f)
                z1 = 0.0f;
            return output;
        }
    };
    // Two stages per side (L/R), prime-number delays for maximum decorrelation
    StereoizerAllpass stereoizerL[2];
    StereoizerAllpass stereoizerR[2];

    // Plugin variables
    int ReverbEnabled = 0;
    float ReverbAmount = 0.0f;

    // Recovery stage filters (applied after spring tank output)
    // Design: Transducer resonance - inductive pickup coil creates resonant peak ~2.5kHz
    float sampleRate = 48000.0f;

    // Peaking EQ / recovery filters for the spring tank.
    //
    // Coefficients AND state are double; only the sample interface is float.
    // In float32 the same recursion measures (time domain, 2M samples of
    // constant input, the real recursion rather than an analytic evaluation):
    //
    //   wet high-pass DC rejection, float32   float64
    //     20 Hz @ 48 kHz     8.3e-3          1.5e-14
    //     20 Hz @ 96 kHz     9.5e-3          3.0e-14
    //     60 Hz @ 192 kHz    1.1e-2          6.0e-14
    //
    // i.e. in float32 the high-pass passes DC at about -39 dB instead of
    // rejecting it. The low-pass DC gain and the high-pass passband gain are
    // correct to within 0.001 dB in either precision at every cutoff the
    // reverb configures, so this is a DC-rejection issue, not a level error.
    //
    // Flushing the OUTPUT with `if (std::abs(y) < 1e-15f) y = 0.0f;` would not
    // prevent denormals: 1e-15 is a perfectly normal float (denormals start
    // near 1e-38), and the recirculating state would stay untouched. The state
    // is flushed instead, which is where a denormal would actually persist.
    struct BiquadState
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        void reset() { z1 = z2 = 0.0; }

        float process(float xf)
        {
            // Direct Form II (transposed) biquad
            const double x = static_cast<double>(xf);
            const double y = b0 * x + z1;
            z1 = b1 * x + z2 - a1 * y;
            z2 = b2 * x - a2 * y;
            // Flush the recirculating state, not the output.
            if (std::abs(z1) < 1.0e-300)
                z1 = 0.0;
            if (std::abs(z2) < 1.0e-300)
                z2 = 0.0;
            return static_cast<float>(y);
        }
    };

    // Recovery filters for the mono spring tank (transducer voicing + user
    // wet low/high cut). Cloud applies the user cuts internally.
    BiquadState classicTransducerResonance;
    BiquadState classicImpedanceRolloff;
    BiquadState classicWetHighPass;

    // Engine changes render the one prepared Spring and one prepared Cloud in
    // parallel for an exact sample-time interval. With two engine choices, a
    // request during the transition can only keep direction or reverse it.
    static constexpr float kEngineSwitchSeconds = 0.050f;
    bool engineTransitionActive = false;
    int transitionSampleIndex = 0;  // Next sample's target mix position.
    int transitionSamples = 2400;

    // The inaudible outgoing engine receives zero input after the crossfade.
    // Its native decay is preserved through the maximum pre-delay plus the
    // reversal hold, then shortened so the hidden second-engine CPU cost is
    // bounded without a large audio-thread buffer clear.
    static constexpr float kDrainSilenceThreshold = 1.0e-5f;
    static constexpr float kDrainHoldSeconds = 0.10f;
    static constexpr float kMaximumPreDelaySeconds = 0.20f;
    int drainingEngine = -1;
    int drainSilentSamples = 0;
    int drainHoldSamples = 4800;
    int drainElapsedSamples = 0;
    int drainMinimumSamples = 14400;
    bool drainDecayShortened = false;
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    bool debugDrainNonFiniteOnce = false;
#endif

    std::array<SmoothedReverbParams, 2> engineParams{};
    std::array<bool, 2> engineParamsInitialized{{false, false}};
    std::array<bool, 2> engineNoiseEnabled{{false, false}};
    std::array<float, 2> engineNoiseAmount{{0.0f, 0.0f}};
    bool smoothingInitialized = false;
    int lastProcessBlockSize = 512;

    // Both scratch paths are fully allocated in Reset(). Index is engine.
    std::array<std::vector<std::vector<float>>, 2> engineScratch;
    int preparedChannels = 2;
    int preparedMaxBlockSize = 512;

    // Configure peaking EQ: fc = center freq, gainDb = boost, Q = bandwidth
    void configurePeakingEQ(BiquadState& state, float fc, float gainDb, float Q);

    // Configure 2nd-order HPF (Butterworth-style)
    void configureHPF(BiquadState& state, float fc, bool resetState = true);

    // Configure 2nd-order LPF for impedance rolloff
    void configureLPF(BiquadState& state, float fc, bool resetState = true);

    // Reset the width stereoizer allpass state.
    void resetStereoizerState();
    void clearEngineState(int engine);
    void clearReverbState();
    void cancelEngineTransition(int activeEngine);
    void startEngineTransition(int targetEngine);
    void applyEngineParameters(int engine, bool allowInternalNoise, bool shortenDecay = false);
    void processEngine(int engine, std::vector<std::vector<float>>& workBuffer, int processChannels, int blockSize);
    void copyInputToScratch(const std::vector<std::vector<float>>& input, int engine, int processChannels,
                            int inputOffset, int blockSize);
    void updateDrainSilence(const std::vector<std::vector<float>>& drained, int engine, int processChannels,
                            int blockSize);
};
