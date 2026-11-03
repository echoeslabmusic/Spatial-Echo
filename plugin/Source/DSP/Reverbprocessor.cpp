// ReverbProcessor: Multi-engine reverb behind the macro schema (Spring mono tank + width stereoizer /
//   CloudSeed lush).
// Realtime-safety: No allocation/locks in process()

#include <JuceHeader.h>
#include "Reverbprocessor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr float kReverbSizeGamma = 1.25f;
constexpr float kReverbDecayGamma = 1.2f;
constexpr float kReverbDampingGamma = 1.3f;
constexpr float kWetLowCutMinHz = 20.0f;
constexpr float kWetLowCutMaxHz = 600.0f;
constexpr float kWetHighCutMinHz = 1000.0f;
constexpr float kWetHighCutMaxHz = 16000.0f;
constexpr float kDefaultWetLowCutHz = 20.0f;
constexpr float kDefaultWetHighCutHz = 8000.0f;

// ---- Macro laws ---------------------------------------------------------
// Calibrated so that macro mid-positions reproduce the reference engine
// voicing: Tone 0.5 → damping 0.564 / high-cut 8 kHz / low-cut 20 Hz;
// Character 0.5 → balance 0.5, scatter 0.375, diffusion 0.50;
// Size 0.7085 → early/late 0.70. Mod rate is fixed in the 0.5-1 Hz
// artifact-smoothing band both engines are voiced in.
constexpr float kToneDampingScale = 1.128f;
constexpr float kToneHighCutRefHz = 8000.0f;
constexpr float kToneHighCutOctaves = 2.2f;  // ±1.1 octaves around 8 kHz
constexpr float kToneLowCutRiseHz = 260.0f;  // bright half raises low cut to 150 Hz
constexpr float kCharScatterBase = 0.10f;
constexpr float kCharScatterRange = 0.55f;
constexpr float kCharDiffusionBase = 0.15f;
constexpr float kCharDiffusionRange = 0.70f;
constexpr float kSizeEarlyLateBase = 0.35f;
constexpr float kSizeEarlyLateRange = 0.50f;
constexpr float kFixedModRateHz = 0.6f;
constexpr int kCloudLineCount = 8;  // fixed: discrete internal steps
                                    // would click under automation
// Spring width stereoizer: side gain at full width. Side is built from the
// difference of two decorrelated tank copies, so mono fold-down (L+R) is
// always the pure tank regardless of width.
constexpr float kSpringWidthSideGain = 0.9f;
constexpr float kSpringInternalDrive = 1.0f;

// Per-engine output normalisation. These are the ONLY level trims in the
// reverb, and they exist so the processor presents a defined contract to the
// routing code: at presentation amount 1.0, input-to-output gain is 0 dB
// in-band (200 Hz - 4 kHz multitone) at the default macro point.
//
// Measured with ReverbInsertionGain at 48 kHz, default macros
// (size .7085, decay .6802, tone .5, mod .08, character .5, width .4):
//   Spring  -2.57 dB at a trim of 0.967  ->  0.967 * 10^(2.57/20) = 1.301
//   Cloud   -1.08 dB at a trim of 1.000  ->  1.000 * 10^(1.08/20) = 1.133
//
// These are the only level trims in the reverb path. There is no separate
// Character->dB compensation table and no makeup pair in EchoesModel: the
// engine-internal gains are correct at source (SpringDriverSaturation's
// emphasis pair is matched, CloudSeed's early/late fader is level-preserving,
// and both engines share one T60 law).
constexpr float kSpringPostCalibration = 1.301f;
constexpr float kCloudPostCalibration = 1.133f;

inline float shapeReverbParam(float x, float gamma)
{
    x = std::clamp(x, 0.0f, 1.0f);
    return std::pow(x, gamma);
}

inline float smoothingAlpha(float smoothingMs, float sampleRate, int blockSize)
{
    const float sr = std::max(1.0f, sampleRate);
    const float block = static_cast<float>(std::max(1, blockSize));
    const float tauSamples = std::max(1.0f, smoothingMs * 0.001f * sr);
    return 1.0f - std::exp(-block / tauSamples);
}

inline void smoothValue(float& state, float target, float alpha)
{
    state += alpha * (target - state);
}

inline void sanitizeWetCutoffs(float& lowCutHz, float& highCutHz, float sampleRate)
{
    const float nyquistSafe = 0.45f * std::max(1.0f, sampleRate);
    const float maxHigh = std::min(kWetHighCutMaxHz, nyquistSafe);
    highCutHz = std::clamp(highCutHz, kWetHighCutMinHz, maxHigh);
    const float maxLow = std::min(kWetLowCutMaxHz, std::max(kWetLowCutMinHz, highCutHz - 20.0f));
    lowCutHz = std::clamp(lowCutHz, kWetLowCutMinHz, maxLow);
    if (lowCutHz >= highCutHz)
        lowCutHz = std::max(kWetLowCutMinHz, highCutHz * 0.5f);
}
}  // namespace

ReverbProcessor::ReverbProcessor() = default;
ReverbProcessor::~ReverbProcessor() = default;

void ReverbProcessor::resetStereoizerState()
{
    for (auto& d : stereoizerL)
        d.reset();
    for (auto& d : stereoizerR)
        d.reset();
}

void ReverbProcessor::clearEngineState(int engine)
{
    if (engine == 0)
    {
        if (ClassicReverb)
            ClassicReverb->clearAudioState();

        classicTransducerResonance.reset();
        classicImpedanceRolloff.reset();
        classicWetHighPass.reset();
        resetStereoizerState();
        return;
    }

    if (engine == 1 && cloudReverb)
        cloudReverb->clearBuffers();
}

void ReverbProcessor::clearReverbState()
{
    clearEngineState(0);
    clearEngineState(1);
}

void ReverbProcessor::hardClearNow()
{
    clearReverbState();
    cancelEngineTransition(ReverbEngine);
    // Parameters remain installed in both prepared engines. The normal
    // UpdateParameters call at the start of the next block resumes any
    // interrupted structural morph without doing work in this safety clear.
}

// Configure peaking EQ using RBJ Audio EQ Cookbook formulas
void ReverbProcessor::configurePeakingEQ(BiquadState& state, float fc, float gainDb, float Q)
{
    // Derived in double to match BiquadState's double state; see the note there.
    constexpr double pi = juce::MathConstants<double>::pi;
    double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);  // sqrt of linear gain
    double w0 = 2.0 * pi * static_cast<double>(fc) / static_cast<double>(sampleRate);
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * static_cast<double>(Q));

    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cosw0;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha / A;

    // Normalize by a0
    state.b0 = b0 / a0;
    state.b1 = b1 / a0;
    state.b2 = b2 / a0;
    state.a1 = a1 / a0;
    state.a2 = a2 / a0;
    state.reset();
}

// Configure 2nd-order HPF (Butterworth-style)
void ReverbProcessor::configureHPF(BiquadState& state, float fc, bool resetState)
{
    constexpr double pi = juce::MathConstants<double>::pi;
    double w0 = 2.0 * pi * static_cast<double>(fc) / static_cast<double>(sampleRate);
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * 0.707);  // Q = 0.707 for Butterworth

    double b0 = (1.0 + cosw0) / 2.0;
    double b1 = -(1.0 + cosw0);
    double b2 = (1.0 + cosw0) / 2.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha;

    // Normalize by a0
    state.b0 = b0 / a0;
    state.b1 = b1 / a0;
    state.b2 = b2 / a0;
    state.a1 = a1 / a0;
    state.a2 = a2 / a0;
    if (resetState)
        state.reset();
}

// Configure 2nd-order LPF (Butterworth-style) for impedance rolloff
void ReverbProcessor::configureLPF(BiquadState& state, float fc, bool resetState)
{
    constexpr double pi = juce::MathConstants<double>::pi;
    double w0 = 2.0 * pi * static_cast<double>(fc) / static_cast<double>(sampleRate);
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * 0.707);  // Q = 0.707 for Butterworth

    double b0 = (1.0 - cosw0) / 2.0;
    double b1 = 1.0 - cosw0;
    double b2 = (1.0 - cosw0) / 2.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha;

    // Normalize by a0
    state.b0 = b0 / a0;
    state.b1 = b1 / a0;
    state.b2 = b2 / a0;
    state.a1 = a1 / a0;
    state.a2 = a2 / a0;
    if (resetState)
        state.reset();
}

void ReverbProcessor::Reset(float sampleRateHz, int numChannels, int maxBlockSize)
{
    this->sampleRate = sampleRateHz;
    smoothingInitialized = false;
    engineParamsInitialized = {{false, false}};
    engineNoiseEnabled = {{false, false}};
    engineNoiseAmount = {{0.0f, 0.0f}};
    preparedChannels = std::max(1, numChannels);
    preparedMaxBlockSize = std::max(1, maxBlockSize);
    lastProcessBlockSize = preparedMaxBlockSize;
    ReverbEngine = 0;
    activeReverbEngine = 0;
    transitionFromEngine = 0;
    transitionToEngine = 0;
    engineTransitionActive = false;
    transitionSampleIndex = 0;
    transitionSamples = std::max(2, static_cast<int>(std::lround(sampleRateHz * kEngineSwitchSeconds)));
    drainingEngine = -1;
    drainSilentSamples = 0;
    drainHoldSamples = std::max(1, static_cast<int>(std::lround(sampleRateHz * kDrainHoldSeconds)));
    drainElapsedSamples = 0;
    drainMinimumSamples = std::max(
        drainHoldSamples, static_cast<int>(std::lround(sampleRateHz * (kMaximumPreDelaySeconds + kDrainHoldSeconds))));
    drainDecayShortened = false;
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
    debugDrainNonFiniteOnce = false;
#endif
    for (auto& scratch : engineScratch)
        scratch.assign(static_cast<size_t>(preparedChannels),
                       std::vector<float>(static_cast<size_t>(preparedMaxBlockSize), 0.0f));

    // Spring engine: single shared mono tank (real tanks are mono).
    // Reuse the existing instance across re-prepares. Constructing a
    // replacement with make_unique would build the new tank *before* releasing
    // the old one, so a sample-rate or block-size change would transiently hold
    // two complete engines. Cloud's delay buffers are heap vectors sized from
    // the engine sample rate at prepare time (sizeof 0.44 MB; heap 52.8 MB at
    // 48 kHz, 210.0 MB at 192 kHz), and that heap total is the number that
    // would double, so reusing the instance matters. reset() below re-prepares
    // every buffer.
    if (!ClassicReverb)
        ClassicReverb = std::make_unique<WaveguideSpringReverbNetwork>();
    ClassicReverb->reset(sampleRateHz, preparedMaxBlockSize);

    // Recovery stage filters for the tank output.
    // Design: Transducer resonance peak at ~2.5kHz with +4dB boost, Q=1.5
    configurePeakingEQ(classicTransducerResonance, 2500.0f, 4.0f, 1.5f);
    configureHPF(classicWetHighPass, kDefaultWetLowCutHz);
    configureLPF(classicImpedanceRolloff, kDefaultWetHighCutHz);

    // Cloud engine initialization. Reused across re-prepares for the same
    // reason as the Spring tank above; CloudSeedReverb::reset() re-runs
    // SetSamplerate and ClearBuffers, which fully re-initialises the engine.
    if (!cloudReverb)
        cloudReverb = std::make_unique<CloudSeedReverb>();
    cloudReverb->reset(sampleRateHz, preparedMaxBlockSize);

    // Spring width stereoizer: two modulated allpass stages per side,
    // prime-number delays for maximum decorrelation.
    //                        delayMs  modMs  rateHz  feedback
    stereoizerL[0].prepare(sampleRateHz, 7.0f, 0.5f, 0.7f, 0.45f);
    stereoizerL[1].prepare(sampleRateHz, 11.0f, 0.4f, 1.1f, 0.40f);
    stereoizerR[0].prepare(sampleRateHz, 5.0f, 0.6f, 0.9f, 0.45f);
    stereoizerR[1].prepare(sampleRateHz, 13.0f, 0.3f, 0.6f, 0.40f);
}

void ReverbProcessor::UpdateParameters(int reverbEnabled, float reverbAmount, const ReverbMacroParams& macros,
                                       bool noiseEnabled, float noiseAmount)
{
    const bool hadInitializedSmoothing = smoothingInitialized;
    const int requestedReverbEngine = std::clamp(macros.engine, 0, 1);
    const bool wasAudible = (ReverbEnabled != 0) && (ReverbAmount > 0.0f);

    ReverbEnabled = reverbEnabled;
    ReverbAmount = reverbAmount;
    ReverbEngine = requestedReverbEngine;
    const bool nowAudible = (ReverbEnabled != 0) && (ReverbAmount > 0.0f);
    const bool disabledAfterAudible = hadInitializedSmoothing && wasAudible && !nowAudible;
    const bool hardClear = disabledAfterAudible;

    // ---- Macro laws: derive engine-native parameters ----
    const float size = std::clamp(macros.size, 0.0f, 1.0f);
    const float decay = std::clamp(macros.decay, 0.0f, 1.0f);
    const float tone = std::clamp(macros.tone, 0.0f, 1.0f);
    const float mod = std::clamp(macros.mod, 0.0f, 1.0f);
    const float character = std::clamp(macros.character, 0.0f, 1.0f);
    const float preDelay = std::clamp(macros.preDelay, 0.0f, 1.0f);
    const float width = std::clamp(macros.width, 0.0f, 1.0f);

    const float damping = std::clamp(kToneDampingScale * (1.0f - tone), 0.0f, 1.0f);
    float highCutHz = kToneHighCutRefHz * std::pow(2.0f, kToneHighCutOctaves * (tone - 0.5f));
    // tone 0.5 → 20 Hz; tone 1.0 → 20 + 0.5·260 = 150 Hz
    float lowCutHz = 20.0f + std::max(0.0f, tone - 0.5f) * kToneLowCutRiseHz;

    const float springBalance = character;
    const float scatterAmount = kCharScatterBase + kCharScatterRange * character;
    const float diffusion = kCharDiffusionBase + kCharDiffusionRange * character;
    const float earlyLateMix = kSizeEarlyLateBase + kSizeEarlyLateRange * size;
    const float modDepth = mod;
    const float modRate = kFixedModRateHz;

    // Perceptual tapers for a more musical control feel
    const float sizeShaped = shapeReverbParam(size, kReverbSizeGamma);
    const float decayShaped = shapeReverbParam(decay, kReverbDecayGamma);
    const float dampingShaped = shapeReverbParam(damping, kReverbDampingGamma);

    // Keep engine-native drive fixed and let explicit post-engine calibration
    // own the audible parity work. Route code must not alter engine drive.
    const float presentationGainTarget = reverbAmount;

    float wetLowCutHz = lowCutHz;
    float wetHighCutHz = highCutHz;
    sanitizeWetCutoffs(wetLowCutHz, wetHighCutHz, sampleRate);

    SmoothedReverbParams target;
    target.size = sizeShaped;
    target.decay = decayShaped;
    target.damping = dampingShaped;
    target.diffusion = diffusion;
    target.modRate = modRate;
    target.modDepth = modDepth;
    target.springBalance = springBalance;
    target.scatterAmount = scatterAmount;
    target.preDelay = preDelay;
    target.earlyLateMix = earlyLateMix;
    target.presentationGain = presentationGainTarget;
    target.lowCutHz = wetLowCutHz;
    target.highCutHz = wetHighCutHz;
    target.width = width;

    auto& current = engineParams[static_cast<size_t>(requestedReverbEngine)];
    if (!engineParamsInitialized[static_cast<size_t>(requestedReverbEngine)])
    {
        current = target;
        engineParamsInitialized[static_cast<size_t>(requestedReverbEngine)] = true;
    }
    else
    {
        const float structuralAlpha = smoothingAlpha(kStructuralSmoothingMs, sampleRate, lastProcessBlockSize);
        const float controlAlpha = smoothingAlpha(kControlSmoothingMs, sampleRate, lastProcessBlockSize);

        smoothValue(current.size, target.size, structuralAlpha);
        smoothValue(current.diffusion, target.diffusion, structuralAlpha);
        smoothValue(current.preDelay, target.preDelay, structuralAlpha);
        smoothValue(current.decay, target.decay, controlAlpha);
        smoothValue(current.damping, target.damping, controlAlpha);
        smoothValue(current.modRate, target.modRate, controlAlpha);
        smoothValue(current.modDepth, target.modDepth, controlAlpha);
        smoothValue(current.springBalance, target.springBalance, controlAlpha);
        smoothValue(current.scatterAmount, target.scatterAmount, controlAlpha);
        smoothValue(current.earlyLateMix, target.earlyLateMix, controlAlpha);
        smoothValue(current.presentationGain, target.presentationGain, controlAlpha);
        smoothValue(current.lowCutHz, target.lowCutHz, controlAlpha);
        smoothValue(current.highCutHz, target.highCutHz, controlAlpha);
        smoothValue(current.width, target.width, controlAlpha);
    }

    engineNoiseEnabled[static_cast<size_t>(requestedReverbEngine)] = noiseEnabled;
    engineNoiseAmount[static_cast<size_t>(requestedReverbEngine)] = noiseAmount;
    smoothingInitialized = true;

    if (hardClear)
    {
        clearReverbState();
        cancelEngineTransition(requestedReverbEngine);
    }
    else if (!hadInitializedSmoothing || !nowAudible || !wasAudible)
        cancelEngineTransition(requestedReverbEngine);
    else
        startEngineTransition(requestedReverbEngine);

    applyEngineParameters(requestedReverbEngine, true);
}

void ReverbProcessor::cancelEngineTransition(int activeEngine)
{
    const int clampedEngine = std::clamp(activeEngine, 0, 1);
    activeReverbEngine = clampedEngine;
    transitionFromEngine = clampedEngine;
    transitionToEngine = clampedEngine;
    engineTransitionActive = false;
    transitionSampleIndex = 0;
    drainingEngine = -1;
    drainSilentSamples = 0;
    drainElapsedSamples = 0;
    drainDecayShortened = false;
}

void ReverbProcessor::startEngineTransition(int targetEngine)
{
    const int target = std::clamp(targetEngine, 0, 1);

    if (engineTransitionActive)
    {
        if (target == transitionToEngine)
            return;

        if (target == transitionFromEngine)
        {
            if (transitionSampleIndex == 0)
            {
                cancelEngineTransition(transitionFromEngine);
                return;
            }

            std::swap(transitionFromEngine, transitionToEngine);
            transitionSampleIndex = std::clamp(transitionSamples - transitionSampleIndex, 0, transitionSamples - 1);
        }
        return;
    }

    if (target == activeReverbEngine)
        return;

    transitionFromEngine = activeReverbEngine;
    transitionToEngine = target;
    transitionSampleIndex = 0;
    engineTransitionActive = true;

    if (drainingEngine == target)
    {
        drainingEngine = -1;
        drainSilentSamples = 0;
        drainElapsedSamples = 0;
        drainDecayShortened = false;
    }
}

void ReverbProcessor::applyEngineParameters(int engine, bool allowInternalNoise, bool shortenDecay)
{
    if (engine < 0 || engine > 1 || !engineParamsInitialized[static_cast<size_t>(engine)])
        return;

    const auto& params = engineParams[static_cast<size_t>(engine)];
    const float nativeDecay = shortenDecay ? 0.0f : params.decay;
    if (engine == 0 && ClassicReverb)
    {
        float wetLowCut = params.lowCutHz;
        float wetHighCut = params.highCutHz;
        if (wetLowCut >= wetHighCut)
            wetLowCut = std::max(kWetLowCutMinHz, wetHighCut * 0.5f);

        const bool useNoise = allowInternalNoise && engineNoiseEnabled[static_cast<size_t>(engine)];
        ClassicReverb->SetAllParameters(params.size, nativeDecay, params.damping, params.diffusion, params.modRate,
                                        params.modDepth, params.springBalance, params.scatterAmount, params.preDelay,
                                        params.earlyLateMix, kSpringInternalDrive, useNoise,
                                        useNoise ? engineNoiseAmount[static_cast<size_t>(engine)] : 0.0f);
        configureHPF(classicWetHighPass, wetLowCut, false);
        configureLPF(classicImpedanceRolloff, wetHighCut, false);
    }
    else if (engine == 1 && cloudReverb)
    {
        cloudReverb->updateParameters(params.size, nativeDecay, params.damping, params.diffusion, params.modRate,
                                      params.modDepth, params.lowCutHz, params.highCutHz, params.preDelay,
                                      params.earlyLateMix, kCloudLineCount, params.width);
    }
}

void ReverbProcessor::processEngine(int engine, std::vector<std::vector<float>>& workBuffer, int processChannels,
                                    int blockSize)
{
    if (engine < 0 || engine > 1 || processChannels <= 0 || blockSize <= 0)
        return;

    const auto& params = engineParams[static_cast<size_t>(engine)];
    if (engine == 1 && cloudReverb)
    {
        cloudReverb->process(workBuffer, blockSize);

        const float outGain = params.presentationGain * kCloudPostCalibration;
        if (processChannels >= 2)
        {
            const float sideGain = std::min(1.2f, params.width * 1.6f);
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const float mid =
                    0.5f * (workBuffer[0][static_cast<size_t>(sample)] + workBuffer[1][static_cast<size_t>(sample)]);
                const float side =
                    0.5f * (workBuffer[0][static_cast<size_t>(sample)] - workBuffer[1][static_cast<size_t>(sample)]) *
                    sideGain;
                workBuffer[0][static_cast<size_t>(sample)] = (mid + side) * outGain;
                workBuffer[1][static_cast<size_t>(sample)] = (mid - side) * outGain;
            }
            for (int channel = 2; channel < processChannels; ++channel)
                for (int sample = 0; sample < blockSize; ++sample)
                    workBuffer[static_cast<size_t>(channel)][static_cast<size_t>(sample)] *= outGain;
        }
        else
        {
            for (int sample = 0; sample < blockSize; ++sample)
                workBuffer[0][static_cast<size_t>(sample)] *= outGain;
        }
        return;
    }

    if (!ClassicReverb)
        return;

    const float invNumChannels = 1.0f / static_cast<float>(processChannels);
    const float sideGain = params.width * kSpringWidthSideGain;
    // No Character-dependent level compensation is applied here: the spring
    // driver (SpringDriverSaturation) applies its own recovery makeup, so the
    // raw Spring level is flat across Character. Measured from
    // ReverbEngineLevelParityProbe rows, Character 0 -> 1 moves the raw early
    // RMS by 1.22 dB (compact), 0.43 dB (default), 0.74 dB (large).
    const float springCharacterCalibration = params.presentationGain * kSpringPostCalibration;
    const bool stereoOut = processChannels >= 2;

    for (int sample = 0; sample < blockSize; ++sample)
    {
        float monoIn = 0.0f;
        for (int channel = 0; channel < processChannels; ++channel)
            monoIn += workBuffer[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
        monoIn *= invNumChannels;

        float wet = ClassicReverb->processSample(monoIn);
        wet = classicTransducerResonance.process(wet);
        wet = classicWetHighPass.process(wet);
        wet = classicImpedanceRolloff.process(wet);

        if (stereoOut)
        {
            const float decorrelatedL = stereoizerL[1].process(stereoizerL[0].process(wet));
            const float decorrelatedR = stereoizerR[1].process(stereoizerR[0].process(wet));
            const float side = 0.5f * (decorrelatedL - decorrelatedR) * sideGain;
            workBuffer[0][static_cast<size_t>(sample)] = (wet + side) * springCharacterCalibration;
            workBuffer[1][static_cast<size_t>(sample)] = (wet - side) * springCharacterCalibration;
            for (int channel = 2; channel < processChannels; ++channel)
                workBuffer[static_cast<size_t>(channel)][static_cast<size_t>(sample)] =
                    wet * springCharacterCalibration;
        }
        else
        {
            workBuffer[0][static_cast<size_t>(sample)] = wet * springCharacterCalibration;
        }
    }
}

void ReverbProcessor::copyInputToScratch(const std::vector<std::vector<float>>& input, int engine, int processChannels,
                                         int inputOffset, int blockSize)
{
    auto& scratch = engineScratch[static_cast<size_t>(engine)];
    const size_t bytes = static_cast<size_t>(blockSize) * sizeof(float);
    for (int channel = 0; channel < processChannels; ++channel)
    {
        std::memcpy(scratch[static_cast<size_t>(channel)].data(),
                    input[static_cast<size_t>(channel)].data() + inputOffset, bytes);
    }
    for (int channel = processChannels; channel < preparedChannels; ++channel)
    {
        std::fill_n(scratch[static_cast<size_t>(channel)].begin(), blockSize, 0.0f);
    }
}

void ReverbProcessor::updateDrainSilence(const std::vector<std::vector<float>>& drained, int engine,
                                         int processChannels, int blockSize)
{
    const float presentationGain =
        (engine >= 0 && engine <= 1) ? std::abs(engineParams[static_cast<size_t>(engine)].presentationGain) : 1.0f;
    const float inversePresentationGain = 1.0f / std::max(1.0e-12f, presentationGain);

    for (int sample = 0; sample < blockSize; ++sample)
    {
        float peak = 0.0f;
        for (int channel = 0; channel < processChannels; ++channel)
        {
            const float value = drained[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
            if (!std::isfinite(value))
            {
                // The drain is hidden from the top-level output scan. Clear
                // both already-prepared engines here so poisoned state cannot
                // be frozen and reintroduced by a later engine selection.
                hardClearNow();
                return;
            }
            peak = std::max(peak, std::abs(value) * inversePresentationGain);
        }

        drainSilentSamples = (peak <= kDrainSilenceThreshold) ? drainSilentSamples + 1 : 0;
    }

    drainElapsedSamples = std::min(std::numeric_limits<int>::max() - blockSize, drainElapsedSamples) + blockSize;

    // Preserve the exact live state long enough for the complete public
    // pre-delay and a rapid user reversal. Once that protected interval has
    // elapsed, shorten only the inaudible engine's native decay. The stored
    // user parameters remain untouched and are restored before any re-entry.
    if (!drainDecayShortened && drainElapsedSamples >= drainMinimumSamples)
    {
        applyEngineParameters(engine, false, true);
        drainDecayShortened = true;
    }

    // Decide only after inspecting the complete rendered chunk. A delayed
    // response later in the same callback must cancel an earlier quiet hold.
    // The elapsed-time guard also prevents an ordinary small callback from
    // freezing a 200 ms pre-delay before its latent signal can emerge.
    if (drainElapsedSamples >= drainMinimumSamples && drainSilentSamples >= drainHoldSamples)
    {
        // Leave the inactive engine frozen at its stored user parameters, not
        // at the temporary zero-decay drain setting. Restoring at the silent
        // completion boundary makes a later re-entry immediately equivalent
        // to a prepared engine and keeps this work inside allocation tracking.
        applyEngineParameters(engine, false, false);
        drainingEngine = -1;
        drainSilentSamples = 0;
        drainElapsedSamples = 0;
        drainDecayShortened = false;
    }
}

void ReverbProcessor::ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize)
{
    if (blockSize <= 0 || ReverbEnabled == 0 || ReverbAmount <= 0.0f)
        return;

    if (buffer.size() != static_cast<size_t>(preparedChannels))
    {
        jassertfalse;
        for (auto& channel : buffer)
            std::fill_n(channel.begin(), std::min(blockSize, static_cast<int>(channel.size())), 0.0f);
        return;
    }

    const int processChannels = preparedChannels;

    int processBlockSize = blockSize;
    for (int channel = 0; channel < processChannels; ++channel)
        processBlockSize = std::min(processBlockSize, static_cast<int>(buffer[static_cast<size_t>(channel)].size()));

    if (processBlockSize < blockSize)
        jassertfalse;
    if (processBlockSize <= 0)
        return;

    blockSize = processBlockSize;
    lastProcessBlockSize = std::max(1, blockSize);

    if (ClassicReverb)
    {
        ClassicReverb->SetRealtimeContext(sampleRate, blockSize);
    }

    int offset = 0;
    while (offset < blockSize)
    {
        int chunk = std::min(preparedMaxBlockSize, blockSize - offset);
        if (engineTransitionActive)
            chunk = std::min(chunk, transitionSamples - transitionSampleIndex);

        if (chunk <= 0)
        {
            jassertfalse;
            break;
        }

        if (engineTransitionActive)
        {
            copyInputToScratch(buffer, transitionFromEngine, processChannels, offset, chunk);
            copyInputToScratch(buffer, transitionToEngine, processChannels, offset, chunk);
            processEngine(transitionFromEngine, engineScratch[static_cast<size_t>(transitionFromEngine)],
                          processChannels, chunk);
            processEngine(transitionToEngine, engineScratch[static_cast<size_t>(transitionToEngine)], processChannels,
                          chunk);

            const float denominator = static_cast<float>(transitionSamples - 1);
            for (int sample = 0; sample < chunk; ++sample)
            {
                const float targetMix = static_cast<float>(transitionSampleIndex + sample) / denominator;
                const float sourceGain = std::sqrt(std::max(0.0f, 1.0f - targetMix));
                const float targetGain = std::sqrt(std::max(0.0f, targetMix));
                for (int channel = 0; channel < processChannels; ++channel)
                {
                    buffer[static_cast<size_t>(channel)][static_cast<size_t>(offset + sample)] =
                        engineScratch[static_cast<size_t>(transitionFromEngine)][static_cast<size_t>(channel)]
                                     [static_cast<size_t>(sample)] *
                            sourceGain +
                        engineScratch[static_cast<size_t>(transitionToEngine)][static_cast<size_t>(channel)]
                                     [static_cast<size_t>(sample)] *
                            targetGain;
                }
            }

            transitionSampleIndex += chunk;
            if (transitionSampleIndex >= transitionSamples)
            {
                const int outgoingEngine = transitionFromEngine;
                activeReverbEngine = transitionToEngine;
                transitionFromEngine = activeReverbEngine;
                transitionToEngine = activeReverbEngine;
                engineTransitionActive = false;
                transitionSampleIndex = 0;
                drainingEngine = (outgoingEngine == activeReverbEngine) ? -1 : outgoingEngine;
                drainSilentSamples = 0;
                drainElapsedSamples = 0;
                drainDecayShortened = false;
                if (drainingEngine >= 0)
                    applyEngineParameters(drainingEngine, false);
            }
        }
        else
        {
            copyInputToScratch(buffer, activeReverbEngine, processChannels, offset, chunk);
            processEngine(activeReverbEngine, engineScratch[static_cast<size_t>(activeReverbEngine)], processChannels,
                          chunk);
            for (int channel = 0; channel < processChannels; ++channel)
            {
                std::memcpy(buffer[static_cast<size_t>(channel)].data() + offset,
                            engineScratch[static_cast<size_t>(activeReverbEngine)][static_cast<size_t>(channel)].data(),
                            static_cast<size_t>(chunk) * sizeof(float));
            }

            if (drainingEngine >= 0)
            {
                const int engineBeingDrained = drainingEngine;
                auto& drainScratch = engineScratch[static_cast<size_t>(engineBeingDrained)];
                for (int channel = 0; channel < preparedChannels; ++channel)
                    std::fill_n(drainScratch[static_cast<size_t>(channel)].begin(), chunk, 0.0f);
                processEngine(engineBeingDrained, drainScratch, processChannels, chunk);
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
                if (debugDrainNonFiniteOnce)
                {
                    drainScratch[0][0] = std::numeric_limits<float>::quiet_NaN();
                    debugDrainNonFiniteOnce = false;
                }
#endif
                updateDrainSilence(drainScratch, engineBeingDrained, processChannels, chunk);
            }
        }

        offset += chunk;
    }
}
