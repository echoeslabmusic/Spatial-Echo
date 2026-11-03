// ImprovedSpringReverb: Waveguide spring-reverb tank: three coupled springs with stretched allpass dispersion, an
//   8-line Hadamard FDN with time-varying feedback, frequency-dependent decay (separate low/high T60), modulated
//   allpass diffusers, driver saturation and a hum/noise floor.
// Realtime-safety: No allocation in process()
// Based on: Välimäki/Parker/Abel parametric spring model; Parker 2011 dispersion structures; Schlecht/Habets
//   time-varying FDN; Chowdhury WDF implementation patterns

#pragma once

#include "HadamardFDN.h"
#include "StretchedAllpass.h"
#include "ReflectionNetwork.h"
#include "PerSampleOversampler.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <vector>

/**
 * Modulated Allpass Filter (anti-metallic diffusion)
 *
 * Based on modulated allpass diffusion pattern.
 * Small delay modulation breaks up periodic resonances that cause
 * metallic coloration in spring reverb.
 *
 * Uses allpass-interpolated modulation for smooth pitch variation.
 */
class ModulatedAllpass
{
public:
    ModulatedAllpass() = default;

    void prepare(float sampleRate, int maxDelaySamples = 1024)
    {
        fs = sampleRate;
        buffer.resize(static_cast<size_t>(maxDelaySamples), 0.0f);
        bufferSize = maxDelaySamples;
        writeIdx = 0;
        z1 = 0.0f;
        lfoSin = 0.0f;
        lfoCos = 1.0f;
        lfoNormCounter = 0;
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writeIdx = 0;
        z1 = 0.0f;
        lfoSin = 0.0f;
        lfoCos = 1.0f;
        lfoNormCounter = 0;
    }

    /**
     * Set modulated allpass parameters
     * @param delaySamples base delay length
     * @param feedback allpass coefficient (-0.7 to 0.7)
     * @param modDepthSamples modulation depth in samples (1-8 typical)
     * @param modRateHz modulation rate in Hz (0.5-2.0 typical)
     */
    void setParams(float delaySamples, float feedback, float modDepthSamples, float modRateHz)
    {
        baseDelay = delaySamples;
        g = feedback;
        if (g < -0.7f)
            g = -0.7f;
        if (g > 0.7f)
            g = 0.7f;
        modDepth = modDepthSamples;
        modRate = modRateHz;
        const float stepRad = juce::MathConstants<float>::twoPi * modRate / fs;
        lfoStepSin = std::sin(stepRad);
        lfoStepCos = std::cos(stepRad);
    }

    float processSample(float input)
    {
        // LFO generates modulation offset; use a recursive oscillator to avoid per-sample trig.
        float lfo = 0.0f;
        if (modDepth > 0.0f && modRate > 0.0f)
        {
            lfo = lfoSin;
            const float newSin = lfoSin * lfoStepCos + lfoCos * lfoStepSin;
            const float newCos = lfoCos * lfoStepCos - lfoSin * lfoStepSin;
            lfoSin = newSin;
            lfoCos = newCos;

            // Periodically renormalize to control numerical drift.
            if (++lfoNormCounter >= 1024)
            {
                const float mag = std::sqrt(lfoSin * lfoSin + lfoCos * lfoCos);
                if (mag > 0.0f)
                {
                    const float invMag = 1.0f / mag;
                    lfoSin *= invMag;
                    lfoCos *= invMag;
                }
                lfoNormCounter = 0;
            }
        }

        float modOffset = lfo * modDepth;
        float totalDelay = baseDelay + modOffset;
        // The allpass reader below biases its fractional part into [0.5, 1.5)
        // and therefore reads one sample further back than totalDelay.
        if (totalDelay < 2.0f)
            totalDelay = 2.0f;
        if (totalDelay > static_cast<float>(bufferSize - 3))
            totalDelay = static_cast<float>(bufferSize - 3);

        // First-order-allpass fractional delay; see InterpolatedDelayLine::allpassRead
        // for the derivation. The simpler form
        //   z1 = buffer[B] + frac * (buffer[A] - z1)
        // inverts the delay inside every integer bin and jumps two whole
        // samples at each bin boundary, which on a modulated diffuser is a
        // periodic click rather than a smooth sweep.
        int intDelay = static_cast<int>(totalDelay);
        float frac = totalDelay - static_cast<float>(intDelay);
        if (frac < 0.5f)
        {
            intDelay -= 1;
            frac += 1.0f;
        }

        int readA = wrapIndex(writeIdx - intDelay);
        int readB = readA - 1;
        if (readB < 0)
            readB += bufferSize;

        const float eta = (1.0f - frac) / (1.0f + frac);
        z1 = eta * buffer[static_cast<size_t>(readA)] + buffer[static_cast<size_t>(readB)] - eta * z1;
        // Denormal flush: z1 recirculates and can hold denormals indefinitely
        // on silent input (same guard as ReverbProcessor::StereoizerAllpass / BiquadState).
        if (std::abs(z1) < 1e-15f)
            z1 = 0.0f;

        // Standard allpass structure
        float v = input + g * z1;
        buffer[static_cast<size_t>(writeIdx)] = v;
        ++writeIdx;
        if (writeIdx >= bufferSize)
            writeIdx = 0;

        return z1 - g * v;
    }

private:
    int wrapIndex(int idx) const noexcept
    {
        if (idx < 0)
            idx += bufferSize;
        else if (idx >= bufferSize)
            idx -= bufferSize;
        return idx;
    }

    float fs = 48000.0f;
    std::vector<float> buffer;
    int bufferSize = 1024;
    int writeIdx = 0;
    float z1 = 0.0f;
    float baseDelay = 100.0f;
    float g = 0.5f;
    float modDepth = 4.0f;
    float modRate = 1.0f;
    float lfoSin = 0.0f;
    float lfoCos = 1.0f;
    float lfoStepSin = 0.0f;
    float lfoStepCos = 1.0f;
    int lfoNormCounter = 0;
};

/**
 * Spring Reverb Hum Generator
 *
 * Classic spring reverb character includes a noise floor with mains hum.
 * The high-gain recovery preamp (~40dB) amplifies 60Hz hum from the
 * high-impedance output transducer acting as a loop antenna.
 *
 * 60Hz hum has predominantly odd harmonics with some even harmonics
 * from rectifier ripple (120Hz, 240Hz).
 *
 * Broadband noise is bandpass filtered to 100Hz-3kHz to match
 * spring reverb bandwidth characteristics.
 */
class SpringReverbHum
{
public:
    SpringReverbHum() = default;

    void prepare(float sampleRate)
    {
        fs = sampleRate;
        phase = 0.0f;
        phaseInc = 60.0f / sampleRate;  // 60Hz fundamental
        initHarmonics();

        // HPF coefficient: standard DC blocker formula
        // y[n] = x[n] - x[n-1] + R * y[n-1] where R = 1 - (2π × fc / fs)
        float hpFreq = 100.0f;
        noiseHpR = 1.0f - (2.0f * juce::MathConstants<float>::pi * hpFreq / fs);
        noiseHpR = std::max(0.9f, std::min(0.9999f, noiseHpR));  // Clamp for stability

        // LPF coefficient: standard 1-pole lowpass
        float lpFreq = 3000.0f;
        noiseLpAlpha = std::exp(-2.0f * juce::MathConstants<float>::pi * lpFreq / fs);

        noiseHpZ = noiseLpZ = noisePrevIn = 0.0f;

        // Simple LCG random seed
        noiseSeed = 12345;
    }

    void reset()
    {
        phase = 0.0f;
        noiseHpZ = noiseLpZ = noisePrevIn = 0.0f;
        initHarmonics();
    }

    /**
     * Set hum/noise parameters
     * @param humLevel 0-1: Level of 60Hz hum and harmonics
     * @param noiseLevel 0-1: Level of broadband noise
     */
    void setParams(float humLevelValue, float noiseLevelValue)
    {
        // Scaled well below unity so the floor stays under the reverb tail in a
        // mix and in impulse-response captures.
        this->humLevel = humLevelValue * 0.0004f;
        this->noiseLevel = noiseLevelValue * 0.0012f;
    }

    void setOutputGain(float outGain)
    {
        gate = (outGain > 0.02f) ? 1.0f : 0.0f;
        // Noise is already scaled by output gain downstream; avoid double-scaling.
        gainScale = 1.0f;
    }

    float processSample()
    {
        if (gate <= 0.0f)
            return 0.0f;

        float hum = 0.0f;
        if (humLevel > 0.0f)
            hum = processHumOscillators();

        // === Broadband Noise (bandpass filtered) ===
        float whiteNoise = fastRand() * 2.0f - 1.0f;

        // HPF at 100Hz using correct DC blocker formula:
        // y[n] = x[n] - x[n-1] + R * y[n-1]
        float hpOut = whiteNoise - noisePrevIn + noiseHpR * noiseHpZ;
        noisePrevIn = whiteNoise;
        noiseHpZ = hpOut;

        // LPF at 3kHz
        noiseLpZ = (1.0f - noiseLpAlpha) * hpOut + noiseLpAlpha * noiseLpZ;

        // Combine hum and noise
        return gate * gainScale * (hum * humLevel + noiseLpZ * noiseLevel);
    }

private:
    struct HarmonicOsc
    {
        float sin = 0.0f;
        float cos = 1.0f;
        float stepSin = 0.0f;
        float stepCos = 1.0f;
        float weight = 1.0f;
    };

    void initHarmonics()
    {
        constexpr float offsets[8] = {0.0f, 0.3f, 0.1f, 0.4f, 0.0f, 0.2f, 0.17f, 0.0f};
        constexpr float weights[8] = {1.0f, 0.3f, 0.5f, 0.15f, 0.2f, 0.08f, 0.06f, 0.05f};
        for (int i = 0; i < 8; ++i)
        {
            const float omega = juce::MathConstants<float>::twoPi * (60.0f * static_cast<float>(i + 1)) / fs;
            oscillators[i].stepSin = std::sin(omega);
            oscillators[i].stepCos = std::cos(omega);
            const float phaseOffset = offsets[i];
            oscillators[i].sin = std::sin(phaseOffset);
            oscillators[i].cos = std::cos(phaseOffset);
            oscillators[i].weight = weights[i];
        }
        oscNormCounter = 0;
    }

    float processHumOscillators()
    {
        float hum = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            hum += oscillators[i].weight * oscillators[i].sin;
            const float newSin =
                oscillators[i].sin * oscillators[i].stepCos + oscillators[i].cos * oscillators[i].stepSin;
            const float newCos =
                oscillators[i].cos * oscillators[i].stepCos - oscillators[i].sin * oscillators[i].stepSin;
            oscillators[i].sin = newSin;
            oscillators[i].cos = newCos;
        }

        if (++oscNormCounter >= 1024)
        {
            for (int i = 0; i < 8; ++i)
            {
                const float mag =
                    std::sqrt(oscillators[i].sin * oscillators[i].sin + oscillators[i].cos * oscillators[i].cos);
                if (mag > 0.0f)
                {
                    const float invMag = 1.0f / mag;
                    oscillators[i].sin *= invMag;
                    oscillators[i].cos *= invMag;
                }
            }
            oscNormCounter = 0;
        }

        return hum;
    }

    float fastRand()
    {
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        return static_cast<float>(noiseSeed) / 4294967296.0f;
    }

    float fs = 48000.0f;
    float phase = 0.0f;
    float phaseInc = 60.0f / 48000.0f;
    float humLevel = 0.01f;
    float noiseLevel = 0.005f;
    float gainScale = 1.0f;

    // Bandpass filter for noise
    float noiseHpR = 0.99f;     // HPF pole coefficient (DC blocker)
    float noiseLpAlpha = 0.8f;  // LPF coefficient
    float noiseHpZ = 0.0f;      // HPF state
    float noiseLpZ = 0.0f;      // LPF state
    float noisePrevIn = 0.0f;   // Previous input for HPF
    uint32_t noiseSeed = 12345;
    float gate = 1.0f;

    HarmonicOsc oscillators[8]{};
    int oscNormCounter = 0;
};

/**
 * TA7200P Driver Saturation Model
 *
 * Spring reverb is CURRENT-DRIVEN, not voltage-driven. The input transducer
 * is a coil wound around an iron core with inductive impedance that rises
 * +6dB/octave above the DC resistance corner frequency.
 *
 * Architecture (per Valve Wizard / Sean Costello):
 *   Input → Pre-emphasis Filter → Nonlinear Function → De-emphasis Filter → Output
 *
 * Pre-emphasis uses a first-order differentiator characteristic (+6dB/octave)
 * to model the voltage rise needed to maintain current through inductive load.
 * This causes HF to clip BEFORE LF (frequency-dependent headroom).
 *
 * Also models transducer core saturation (~2.5 A-T RMS) which affects LF more.
 *
 * Reference: Valve Wizard spring reverb driver analysis, Valhalla DSP
 */
class SpringDriverSaturation
{
public:
    SpringDriverSaturation() = default;

    void prepare(float sampleRate)
    {
        fs = sampleRate;

        // Corner frequency: f_c = R_dc / (2πL)
        // For typical 10Ω input with ~1.27mH inductance: f_c ≈ 1250Hz
        updateFilters();
        reset();
    }

    void reset()
    {
        // Pre-emphasis differentiator state
        prePrevIn = 0.0f;
        // De-emphasis integrator state
        deState = 0.0f;
        // Core saturation integrator state
        coreIntegrator = 0.0f;
        prevCoreSat = 0.0f;
    }

    /**
     * Set saturation parameters
     * @param driveAmount 0-1: Overall drive into saturation
     * @param cornerFreq Hz: Pre-emphasis corner frequency (default 600 Hz; the tank passes
     *                   1250 Hz, the 10Ω/1.27mH input-coil corner)
     * @param bias Asymmetric bias for even harmonics (0 = symmetric)
     */
    void setParams(float driveAmount, float cornerFreqValue = 600.0f, float biasValue = 0.07f)
    {
        drive = 1.0f + driveAmount * 2.5f;  // 1x to ~3.5x drive (less HF emphasis)
        invDrive = 1.0f / drive;
        this->cornerFreq = cornerFreqValue;
        this->bias = biasValue;  // bias in volts-equivalent pre-emphasis domain
        updateFilters();
    }

    float processSample(float input)
    {
        // === Pre-emphasis: first-order HF lift ===
        // Models the voltage rise needed to drive current through the coil's
        // rising inductive impedance, so HF reaches the clipper first.
        //   H_pre(z) = (1 + k) - k·z^-1      (unity at DC, 1 + 2k at Nyquist)
        const float pre = input + preCoeff * (input - prePrevIn);
        prePrevIn = input;

        // === HF Saturation (driver clipping) ===
        float hfSat = softSaturate(pre * drive, bias);

        // === LF Core Saturation (integrator→waveshaper→differentiator) ===
        // Models magnetic core saturation; the core saturates more at LF where
        // flux builds up, and the blend is level-dependent.
        float inputLevel = std::abs(input);
        float coreBlend = std::min(1.0f, inputLevel * 2.0f);

        coreIntegrator = coreLeakCoeff * coreIntegrator + (1.0f - coreLeakCoeff) * input;
        float coreSat = softSaturate(coreIntegrator * drive * 0.7f, 0.0f);
        float coreDiff = coreSat - prevCoreSat;
        prevCoreSat = coreSat;

        float saturated = hfSat * (1.0f - 0.5f * coreBlend) + coreDiff * 0.5f * coreBlend;

        // Hard current limit to emulate driver headroom (~0.35 A eq.).
        // Applied in the driven domain so the input level at which the driver
        // runs out of headroom does not move with the recovery makeup below.
        const float currentLimit = 0.45f;
        saturated = std::max(-currentLimit, std::min(currentLimit, saturated));

        // Recovery-amp makeup: undo the drive gain so the stage is unity for
        // small signals and `drive` only sets where clipping begins.
        saturated *= invDrive;

        // === De-emphasis: EXACT inverse of the pre-emphasis above ===
        //   H_de(z) = 1 / ((1 + k) - k·z^-1)
        //   y[n] = (x[n] + k·y[n-1]) / (1 + k)
        // so the stage's small-signal response is flat by construction and the
        // only frequency dependence is in *where* the nonlinearity engages.
        //
        // A de-emphasis that is not the exact inverse leaves a level and tilt
        // error in this nominally unity-gain stage: a pair of ~1.2 kHz one-pole
        // lowpasses in this position measures ~-22 dB at 6 kHz against a
        // pre-emphasis worth about +1.9 dB there, which is -13.9 dB broadband
        // and -3.5 dB in-band (200 Hz - 4 kHz).
        deState = (saturated + preCoeff * deState) * invOnePlusPreCoeff;
        return deState;
    }

private:
    void updateFilters()
    {
        // Pre-emphasis coefficient: controls differentiation amount.
        // Sample-rate dependent by construction (wc = 2*pi*fc/fs), so the
        // corner tracks the host rate rather than the sample index.
        float wc = 2.0f * juce::MathConstants<float>::pi * cornerFreq / fs;
        preCoeff = std::min(2.0f, wc * 2.0f);  // Scale differentiation by corner freq
        invOnePlusPreCoeff = 1.0f / (1.0f + preCoeff);

        // Core saturation leaky integrator (time constant ~10ms)
        coreLeakCoeff = std::exp(-1.0f / (fs * 0.01f));
    }

    static float softSaturate(float x, float bias)
    {
        // Asymmetric soft saturation with bias for even harmonics
        // tanh-based with configurable asymmetry
        float biased = x + bias;
        float sat;
        if (biased < -3.0f)
            sat = -1.0f;
        else if (biased > 3.0f)
            sat = 1.0f;
        else
        {
            float x2 = biased * biased;
            sat = biased * (27.0f + x2) / (27.0f + 9.0f * x2);
        }
        return sat - softSaturateBias(bias);  // Remove DC offset from bias
    }

    static float softSaturateBias(float bias)
    {
        if (bias < -3.0f)
            return -1.0f;
        if (bias > 3.0f)
            return 1.0f;
        float x2 = bias * bias;
        return bias * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    float fs = 48000.0f;
    float drive = 1.0f;
    float invDrive = 1.0f;
    float cornerFreq = 1250.0f;
    float bias = 0.0f;

    // Pre-emphasis (differentiator) coefficient and state
    float preCoeff = 1.0f;            // Differentiation amount, k
    float invOnePlusPreCoeff = 0.5f;  // 1 / (1 + k), for the inverse de-emphasis
    float prePrevIn = 0.0f;           // Previous input for differentiation

    // De-emphasis (exact inverse of pre-emphasis) state
    float deState = 0.0f;

    // Core saturation state
    float coreLeakCoeff = 0.997f;
    float coreIntegrator = 0.0f;
    float prevCoreSat = 0.0f;
};

/**
 * Single spring simulation with dispersion and reflection network
 */
class SingleSpring
{
public:
    struct Config
    {
        float lengthScale = 1.0f;  // Relative spring length
        float stiffness = 0.5f;    // Higher = more dispersion
        float gain = 1.0f;         // Output level
    };

    SingleSpring() = default;

    void prepare(float sampleRate, int samplesPerBlock)
    {
        fs = sampleRate;

        dispersion.prepare(sampleRate);
        fdn.prepare(sampleRate, samplesPerBlock);
        fdn.setModulatedReadInterpolationMode(performanceModeEnabled
                                                  ? HadamardFDN8::ModulatedReadInterpolationMode::AllpassFast
                                                  : HadamardFDN8::ModulatedReadInterpolationMode::Cubic);
        fdn.setPerformanceModeEnabled(performanceModeEnabled);

        // DC blocker
        dcBlockerZ = 0.0f;
        float dcFreq = 30.0f;
        dcAlpha = std::exp(-2.0f * juce::MathConstants<float>::pi * dcFreq / fs);
    }

    void setPerformanceModeEnabled(bool enabled)
    {
        performanceModeEnabled = enabled;
        fdn.setModulatedReadInterpolationMode(enabled ? HadamardFDN8::ModulatedReadInterpolationMode::AllpassFast
                                                      : HadamardFDN8::ModulatedReadInterpolationMode::Cubic);
        fdn.setPerformanceModeEnabled(enabled);
    }

    void reset()
    {
        dispersion.reset();
        fdn.reset();
        dcBlockerZ = 0.0f;
    }

    void setConfig(const Config& cfg, float t60Low, float t60High, float modRate = 0.7f, float modDepth = 0.12f)
    {
        config = cfg;

        // Set dispersion based on stiffness and length
        const float srRatio = fs / 48000.0f;
        float baseDelay = (4.0f + 8.0f * cfg.stiffness * cfg.lengthScale) * srRatio;
        // Reduced stretch for 36 stages keeps all stages in the 500Hz-6kHz chirp
        // range: 1.05-1.10 leaves ~28-32 useful stages, where 1.15-1.30 with
        // 24 stages leaves only ~12.
        float stretch = 1.05f + 0.05f * cfg.stiffness;
        dispersion.setParams(baseDelay, stretch, cfg.lengthScale);

        // Set FDN for reflections
        // Note: T60 values should NOT be scaled by lengthScale here - the per-line
        // gain calculation in FDN already accounts for different delay lengths
        float fdnSize = 0.3f + 0.5f * cfg.lengthScale;
        // Damping reaches the FDN through t60High; the absorption filters are
        // one-poles defined by their DC and Nyquist gains, with no crossover.
        const float fdnModDepth = performanceModeEnabled ? 0.0f : modDepth;
        fdn.setParams(fdnSize, t60Low, t60High, modRate, fdnModDepth);
    }

    float processSample(float input)
    {
        // DC blocking
        float dcBlocked = input - dcBlockerZ;
        dcBlockerZ = dcAlpha * dcBlockerZ + (1.0f - dcAlpha) * input;

        // Dispersion creates the spring "chirp"
        float dispersed = dispersion.processSample(dcBlocked);

        // FDN creates dense reflections
        float reflections = fdn.processSample(dispersed);

        return reflections * config.gain;
    }

private:
    float fs = 48000.0f;
    Config config;

    StretchedAllpassCascade<36> dispersion;  // 36 stages with reduced stretch for 2.3× more effective chirp stages
    HadamardFDN8 fdn;
    bool performanceModeEnabled = false;

    // DC blocker
    float dcBlockerZ = 0.0f;
    float dcAlpha = 0.99f;
};

/**
 * 2x oversampled waveshaper using JUCE polyphase IIR up/downsampling.
 *
 * Gives the tank's output clipper steeper anti-alias filtering than a linear
 * upsample with one-pole decimation would.
 */
class OversampledTanh2x
{
public:
    OversampledTanh2x() = default;

    // Realtime safety: the spring size morph duplicates a whole prepared
    // processor so the crossfade runs between two coherent reverberant
    // states, and that duplication happens on the audio thread:
    //
    //   WaveguideSpringReverbNetwork::processSample
    //     -> finishSizeMorph / applyParams -> startSizeMorph
    //     -> TripleSpringReverb::copyStateFrom
    //
    // PerSampleOversampler owns a juce::dsp::Oversampling behind a unique_ptr
    // plus two AudioBuffers, so duplicating it means make_unique +
    // initProcessing + setSize - heap allocation inside processSample. Its
    // copy operations are deleted, which is the correct guard.
    //
    // Both processors are always prepared to the same sample rate and block
    // size, so the destination already owns an equivalent oversampler; only
    // its filter history differs. That history is a two-sample half-band
    // state on the tank's final soft-clipper, and the morph crossfade starts
    // at mix = 0 and ramps over 60 ms, so the destination is simply cleared
    // rather than duplicated. Verified inaudible: the morph output is
    // identical to the copy-everything version to within the clipper's own
    // two-sample group delay.
    // Copy CONSTRUCTION is deleted, not defined as a no-op. An empty-body copy
    // constructor would default-construct the oversampler member, and
    // PerSampleOversampler's default constructor calls initialize()
    // (make_unique + initProcessing + setSize). That would leave copy
    // assignment allocation-free while copy construction allocated - the exact
    // shape of the defect this class exists to prevent, one constructor away.
    // Nothing copy-constructs a spring processor: procA/procB are members and
    // the morph path only ever assigns. A future copy construction fails to
    // compile here instead of allocating silently.
    OversampledTanh2x(const OversampledTanh2x&) = delete;
    OversampledTanh2x& operator=(const OversampledTanh2x& other)
    {
        if (this != &other)
            reset();  // clears history only; allocates nothing
        return *this;
    }

    void prepare()
    {
        oversampler.prepare();
        reset();
    }

    void reset() { oversampler.reset(); }

    float process(float input)
    {
        return oversampler.process(input, [](float x) { return fastTanh(x); });
    }

private:
    static float fastTanh(float x)
    {
        if (x < -3.0f)
            return -1.0f;
        if (x > 3.0f)
            return 1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    echoes::PerSampleOversampler2x oversampler;
};

/**
 * Triple-Spring Reverb Processor with Scattering Junctions
 *
 * Reference tank: Accutronics 8AB3C1B 3-spring; springs are counter-wound
 * and connected by coupling rings that create waveguide scattering junctions.
 *
 * Based on Abel, Berners, Costello, Smith AES 2006 paper.
 *
 * Scattering junction equations (lossless condition R + T = 1):
 *   F+_R(s) = T(s)·F+(s) + K_R(s)·F-_R(s)
 *   F-(s) = T_R(s)·F-_R(s) + K(s)·F+(s)
 *
 * Typical spring delay times: 33ms, 37ms, 41ms (SpinSemi FV-1 reference)
 */
class TripleSpringReverb
{
public:
    // Defaults are the classic spring voicing (Accutronics 8AB3C1B) that
    // WaveguideSpringReverbNetwork::reset applies until the first host update.
    struct Params
    {
        float size = 0.60f;          // 0-1: Spring length; slightly short to reduce perceived pre-delay
        float decay = 0.59f;         // 0-1: Decay time, ~2.0 s T60 here
        float damping = 0.4f;        // 0-1: High frequency damping; bright spring character
        float diffusion = 0.45f;     // 0-1: Dispersion/chirp amount; less smear for a clearer drip
        float springBalance = 0.5f;  // 0-1: Balance between springs
        float scatterAmount = 0.3f;  // 0-1: Scattering junction coupling strength
        float modRate = 0.6f;        // Modulation rate in Hz
        float modDepth = 0.08f;      // Modulation depth (0-1); subtle time variation
        float outputGain = 1.0f;     // Downstream gain (for noise gating)
        float preDelay = 0.0f;       // 0-1 maps to 0-200 ms delay before reverb processing
        // Early/late balance between the attack (spring 1 plus the reflection
        // network) and the decay (springs 2 and 3): 0 = all early, 1 = all late.
        float earlyLateMix = 0.7f;
        bool noiseEnabled = false;  // Gate spring hum/noise; follows the global noise toggle
        float noiseAmount = 0.12f;  // 0-1 scalar applied to spring hum/noise
    };

    TripleSpringReverb() = default;

    void prepare(float sampleRate, int samplesPerBlock)
    {
        fs = sampleRate;
        controlUpdateIntervalSamples = std::max(1, static_cast<int>(fs / 2000.0f));
        controlSamplesUntilUpdate = 0;

        // Parameter smoothing coefficient from time constant (sample-rate invariant)
        const float smoothingTimeSec = 0.05f;  // 50 ms target
        const float smoothingDenom = std::max(1.0f, fs * smoothingTimeSec);
        smoothingCoeff = 1.0f - std::exp(-1.0f / smoothingDenom);

        // Decide the topology before the springs are configured, so setConfig
        // bakes in the matching FDN modulation depth and interpolation mode.
        configurePerformanceMode(sampleRate, samplesPerBlock);
        spring1.prepare(sampleRate, samplesPerBlock);
        spring2.prepare(sampleRate, samplesPerBlock);
        spring3.prepare(sampleRate, samplesPerBlock);

        // Input bandpass filter (real springs have limited bandwidth: ~200Hz - 6kHz)
        // HPF at 200Hz: springs don't pass LF well (mechanical HPF behavior)
        // Using standard DC blocker formula: y[n] = x[n] - x[n-1] + R*y[n-1]
        float hpCutoff = 200.0f;
        float wHp = 2.0f * juce::MathConstants<float>::pi * hpCutoff / fs;
        inputHpAlpha = 1.0f - wHp;
        inputHpAlpha = std::max(0.9f, std::min(0.9999f, inputHpAlpha));  // Clamp for stability
        inputHpZ = 0.0f;
        inputHpPrevIn = 0.0f;

        // LPF at 6kHz: natural HF rolloff from mechanical limitations
        float inputBW = 6000.0f;
        float w = 2.0f * juce::MathConstants<float>::pi * inputBW / fs;
        inputLpAlpha = std::exp(-w);
        inputLpZ = 0.0f;

        // Initialize driver saturation
        driverSaturation.prepare(sampleRate);
        driverSaturation.setParams(0.3f);  // Default moderate drive

        // Initialize hum/noise generator (classic noise floor)
        humGenerator.prepare(sampleRate);
        humGenerator.setParams(0.015f, 0.01f);  // Base values; scaled later per outputGain

        // Initialize early reflection network
        earlyReflections.prepare(sampleRate, samplesPerBlock);

        // Initialize modulated allpass diffusers (anti-metallic)
        // Short delays with slight modulation to break up periodic resonances
        const int maxModDelaySamples = static_cast<int>(std::ceil(fs * 0.008f)) + 4;  // ~8ms safety
        modAllpass1.prepare(sampleRate, maxModDelaySamples);
        modAllpass2.prepare(sampleRate, maxModDelaySamples);
        oversampledTanh.prepare();

        // Scattering junction delay lines (coupling rings)
        // Short delays for coupling between springs
        scatterDelay1.prepare(sampleRate, 1024);
        scatterDelay2.prepare(sampleRate, 1024);
        scatterDelay1.setDelay(static_cast<float>(sampleRate) * 0.0005f);                 // 0.5ms
        scatterDelay2.setDelay(static_cast<float>(sampleRate) * 0.0007f);                 // 0.7ms
        scatterLpAlpha = std::exp(-2.0f * juce::MathConstants<float>::pi * 400.0f / fs);  // HF-tilted loss (~0.4 kHz)

        // Pre-delay line: separates the dry signal from the reverb onset for spatial depth.
        // Keep 0-200ms invariant across sample rates.
        const int maxPreDelaySamples = static_cast<int>(std::ceil(sampleRate * 0.2f)) + 8;
        preDelayLine.prepare(sampleRate, maxPreDelaySamples);

        earlyWeightValue = std::cos(currentParams.earlyLateMix * juce::MathConstants<float>::halfPi);
        lateWeightValue = std::sin(currentParams.earlyLateMix * juce::MathConstants<float>::halfPi);
        earlyWeightStep = 0.0f;
        lateWeightStep = 0.0f;

        reset();
    }

    void setPerformanceModeEnabled(bool enabled)
    {
        performanceModeEnabled = enabled;
        spring1.setPerformanceModeEnabled(enabled);
        spring2.setPerformanceModeEnabled(enabled);
        spring3.setPerformanceModeEnabled(enabled);
    }

    // Per-callback context refresh. This runs from ReverbProcessor::ProcessBuffer
    // on every block, so it deliberately does NOT re-decide the performance
    // mode - see configurePerformanceMode below.
    void updateRealtimeContext(float sampleRate) { fs = sampleRate; }

    // Decide the reduced-cost topology once, at prepare time.
    //
    // Re-evaluating this on every callback from updateRealtimeContext, with
    // hysteresis between an "enter" and an "exit" threshold, would let a host
    // varying its callback size around 64-96 samples at 96 kHz flip the tank's
    // topology mid-render, with no crossfade, repeatedly. Crossing the
    // boundary would:
    //   - stop or start spring3 entirely (its whole recursive state freezes
    //     at the last value and is later resumed from that stale state),
    //   - stop or start modAllpass2 the same way,
    //   - switch every FDN's modulated read between cubic and allpass
    //     interpolation,
    //   - swap the late-tail mix between a real spring3 and a 0.6x copy of
    //     spring2,
    //   - and silently change the FDN modulation depth, because setConfig
    //     bakes `performanceModeEnabled ? 0 : modDepth` in and is not re-run.
    //
    // None of that is crossfaded, so each flip is an audible discontinuity in
    // both level and timbre. Latching the decision to prepare() makes the
    // topology a property of the prepared configuration: it can only change
    // when the engine is being re-prepared and reset anyway. The cost of
    // getting it wrong is one-directional and safe - a host that reports a
    // large maximum block but delivers small ones simply runs the full-quality
    // topology and uses more CPU, instead of switching topology under audio.
    void configurePerformanceMode(float sampleRate, int maxBlockSize)
    {
        setPerformanceModeEnabled(sampleRate >= kAdaptiveEnterSampleRate && maxBlockSize <= kAdaptiveEnterBlockSize);
    }

    bool isPerformanceModeEnabled() const noexcept { return performanceModeEnabled; }

    // Duplicate another tank's live reverberant state into this one so a
    // structural retune can be crossfaded between two coherent tanks.
    //
    // Realtime safety: this runs on the audio thread (see the note on
    // OversampledTanh2x). It is allocation-free only because every owning
    // member is already sized identically on both tanks - both are prepared
    // from the same sample rate and block size - so every std::vector
    // assignment reuses existing capacity, and the one member that cannot be
    // duplicated without allocating (the JUCE oversampler inside
    // OversampledTanh2x) is cleared instead of copied.
    //
    // Call this instead of `*a = *b`. The named method exists so the
    // constraint above is visible at the call site rather than buried in an
    // implicitly generated operator=.
    void copyStateFrom(const TripleSpringReverb& other)
    {
        if (this != &other)
            *this = other;
    }

    void reset()
    {
        spring1.reset();
        spring2.reset();
        spring3.reset();
        scatterDelay1.reset();
        scatterDelay2.reset();
        driverSaturation.reset();
        humGenerator.reset();
        earlyReflections.reset();
        modAllpass1.reset();
        modAllpass2.reset();
        oversampledTanh.reset();
        inputHpZ = 0.0f;
        inputHpPrevIn = 0.0f;
        inputLpZ = 0.0f;
        scatterState1 = scatterState2 = 0.0f;
        scatterLpState1 = scatterLpState2 = 0.0f;

        preDelayLine.reset();
        outputGainSmooth = 1.0f;
        noiseAmountSmooth = currentParams.noiseEnabled ? currentParams.noiseAmount : 0.0f;
        controlSamplesUntilUpdate = 0;
        earlyWeightValue = std::cos(currentParams.earlyLateMix * juce::MathConstants<float>::halfPi);
        lateWeightValue = std::sin(currentParams.earlyLateMix * juce::MathConstants<float>::halfPi);
        earlyWeightStep = 0.0f;
        lateWeightStep = 0.0f;
    }

    void setParams(const Params& params)
    {
        currentParams = params;
        currentParams.noiseAmount = std::clamp(currentParams.noiseAmount, 0.0f, 1.0f);
        currentModRate = params.modRate;  // Store for passing to springs
        humGenerator.setOutputGain(params.outputGain);
        const float srRatio = fs / 48000.0f;

        // Pre-delay: the 0-1 parameter maps to 0-200 ms.
        currentPreDelayMs = params.preDelay * 200.0f;  // 0-1 maps to 0-200ms
        float preDelaySamples = (currentPreDelayMs / 1000.0f) * fs;
        preDelayLine.setDelay(preDelaySamples);

        // Calculate T60 from decay (UI maps 0-1 to ~0.4-8.0s)
        constexpr float minT60 = 0.4f;
        constexpr float maxT60 = 8.0f;
        float t60 = minT60 * std::pow(maxT60 / minT60, params.decay);

        // Frequency-dependent decay (Jot 1992, Abel 2006)
        // Real springs/rooms: LF:HF T60 ratio of 2:1 to 5:1
        // damping 0.0 → ratio ~1.2:1 (bright), damping 1.0 → ratio ~3:1 (very damped)
        float dampFactor = 0.3f + 0.7f * params.damping;   // 0.3 to 1.0
        float t60Low = t60 * 1.2f;                         // Bass always decays ~20% slower
        float t60High = t60 * (1.0f - 0.6f * dampFactor);  // HF: 1.0× down to 0.4× of base

        // === Spring Balance: distribute gain among three springs ===
        // springBalance 0.0 = favor spring1 (brighter), 1.0 = favor spring3 (darker)
        // Middle spring always gets base amount to fill midrange
        float bal = params.springBalance;
        // Gains sit in the 0.45-0.65 range to make up for the FDN and dispersion losses.
        float gain1 = 0.65f - 0.20f * bal;  // 0.65 → 0.45 as bal increases
        float gain2 = 0.55f;                // Middle spring boosted for clarity
        float gain3 = 0.45f + 0.20f * bal;  // 0.45 → 0.65 as bal increases
        constexpr float structuralTankGain = 0.55f;

        // Configure spring 1: short spring (33ms @ 48kHz = 1584 samples)
        // Brighter, faster attack
        SingleSpring::Config cfg1;
        cfg1.lengthScale = 0.55f + 0.25f * params.size;  // ~33ms delay
        cfg1.stiffness = 0.5f + 0.35f * params.diffusion;
        // Every spring keeps the same fixed loss inside the coupled tank. The
        // character weights (springGain1-3) are applied once at the final
        // branch mix, so they neither alter the scattering feedback nor count
        // twice in the audible return.
        cfg1.gain = structuralTankGain;
        spring1.setConfig(cfg1, t60Low, t60High, currentModRate, params.modDepth);

        // Configure spring 2: medium spring (37ms @ 48kHz = 1776 samples)
        // Balanced character, fills midrange
        SingleSpring::Config cfg2;
        cfg2.lengthScale = 0.7f + 0.3f * params.size;  // ~37ms delay
        cfg2.stiffness = 0.4f + 0.3f * params.diffusion;
        cfg2.gain = structuralTankGain;
        spring2.setConfig(cfg2, t60Low * 1.1f, t60High * 0.9f, currentModRate, params.modDepth);

        // Configure spring 3: long spring (41ms @ 48kHz = 1968 samples)
        // Darker, deeper resonance
        SingleSpring::Config cfg3;
        cfg3.lengthScale = 0.85f + 0.35f * params.size;  // ~41ms delay
        cfg3.stiffness = 0.3f + 0.25f * params.diffusion;
        cfg3.gain = structuralTankGain;
        spring3.setConfig(cfg3, t60Low * 1.2f, t60High * 0.8f, currentModRate, params.modDepth);

        // Store gains for output mixing
        springGain1 = gain1;
        springGain2 = gain2;
        springGain3 = gain3;

        // Scattering coefficients (lossless: T + R = 1)
        // Higher scatterAmount = more energy exchange between springs
        scatterT = 0.7f - 0.4f * params.scatterAmount;  // Transmission
        scatterR = 1.0f - scatterT;                     // Reflection

        // Keep the driver drive low so the spring IR stays close to linear and
        // HF does not clip at very low input levels.
        float drive = 0.04f + 0.18f * params.scatterAmount;
        drive = std::min(0.30f, std::max(0.02f, drive));
        driverSaturation.setParams(drive, 1250.0f, 0.05f);

        // Configure early reflections network based on T60 and size
        // ReflectionNetwork::setParams(reverbSize, t60, mix, damping)
        // The reflection-network level follows the early side of the early/late crossfade.
        earlyReflectionMix = std::cos(params.earlyLateMix * juce::MathConstants<float>::halfPi);
        earlyReflections.setParams(params.size, t60, earlyReflectionMix, params.damping);

        // Configure modulated allpass diffusers (anti-metallic)
        // Two stages with different delays and modulation rates for decorrelation
        // Delay times: ~1.4-3.6 ms at 48 kHz, subtle modulation depth
        // (2-5 samples, 1.4-3.5 on the second stage)
        float modBaseDelay1 = (67.0f + 40.0f * params.size) * srRatio;   // ~1.4-2.2ms at 48kHz
        float modBaseDelay2 = (113.0f + 60.0f * params.size) * srRatio;  // ~2.4-3.6ms at 48kHz
        float modDepth = (2.0f + 3.0f * params.modDepth) * srRatio;      // 2-5 samples depth @48kHz
        modAllpass1.setParams(modBaseDelay1, 0.55f, modDepth, params.modRate * 0.8f);
        modAllpass2.setParams(modBaseDelay2, -0.45f, modDepth * 0.7f, params.modRate * 1.3f);
    }

    float processSample(float input)
    {
        // Per-sample smoothing only for values that directly multiply the audio output.
        // Structural params (size, decay, damping, etc.) are already block-rate smoothed
        // in ReverbProcessor - a second layer here would add ~50ms sluggishness.
        outputGainSmooth += smoothingCoeff * (currentParams.outputGain - outputGainSmooth);
        const float noiseTarget = currentParams.noiseEnabled ? currentParams.noiseAmount : 0.0f;
        noiseAmountSmooth += smoothingCoeff * (noiseTarget - noiseAmountSmooth);

        if (controlSamplesUntilUpdate <= 0)
        {
            // Update expensive operations at control rate
            currentPreDelayMs = currentParams.preDelay * 200.0f;
            const float preDelaySamples = (currentPreDelayMs / 1000.0f) * fs;
            preDelayLine.setDelay(preDelaySamples);

            const float mixClamped = std::clamp(currentParams.earlyLateMix, 0.0f, 1.0f);
            const float targetEarlyWeight = std::cos(mixClamped * juce::MathConstants<float>::halfPi);
            const float targetLateWeight = std::sin(mixClamped * juce::MathConstants<float>::halfPi);
            const float interval = static_cast<float>(controlUpdateIntervalSamples);
            earlyWeightStep = (targetEarlyWeight - earlyWeightValue) / interval;
            lateWeightStep = (targetLateWeight - lateWeightValue) / interval;

            controlSamplesUntilUpdate = controlUpdateIntervalSamples;
        }

        earlyWeightValue += earlyWeightStep;
        lateWeightValue += lateWeightStep;
        --controlSamplesUntilUpdate;

        // Pre-delay buffers the input, separating the dry and wet onsets.
        preDelayLine.pushSample(input);
        float preDelayedInput = preDelayLine.popSample();

        // === Input Bandpass Filter (200Hz - 6kHz) ===
        // HPF at 200Hz using correct DC blocker formula:
        // y[n] = x[n] - x[n-1] + R*y[n-1]
        float hpOut = preDelayedInput - inputHpPrevIn + inputHpAlpha * inputHpZ;
        inputHpPrevIn = preDelayedInput;
        inputHpZ = hpOut;

        // LPF at 6kHz: natural HF rolloff from mechanical limitations
        inputLpZ = (1.0f - inputLpAlpha) * hpOut + inputLpAlpha * inputLpZ;
        float filtered = inputLpZ;

        // === Early Reflections Network (Householder diffusion) ===
        // Process early reflections BEFORE driver saturation for cleaner attack
        earlyReflections.pushSample(filtered);
        float earlyRefOut = earlyReflections.popSample();

        // === Modulated Allpass Diffusion (anti-metallic) ===
        // Two stages with different modulation rates break up periodic resonances
        // Inserted before driver saturation for maximum effectiveness
        filtered = modAllpass1.processSample(filtered);
        if (!performanceModeEnabled)
            filtered = modAllpass2.processSample(filtered);

        // === TA7200P Driver Saturation ===
        // Pre-emphasis → Saturation → De-emphasis (frequency-dependent clipping)
        filtered = driverSaturation.processSample(filtered);

        // === Scattering Junction Network (Abel et al. AES 2006) ===
        // Springs connected via coupling rings create scattering junctions
        // Energy transfers between springs at these junctions

        // Process spring 1 with scatter feedback from junction 1
        float scatter1In = scatterDelay1.popSample() * scatterR;
        float spring1Out = spring1.processSample(filtered + scatter1In);

        // Scattering junction 1: between spring 1 and spring 2
        // F+_R = T·F+ + R·F-_R (lossless scattering)
        float junction1Out = spring1Out * scatterT + scatterState1 * scatterR;
        scatterState1 = spring1Out;

        // Process spring 2 with scatter input from junction 1 only (no direct input)
        float spring2Out = spring2.processSample(junction1Out);

        float spring3Out = 0.0f;
        if (!performanceModeEnabled)
        {
            // Scattering junction 2: between spring 2 and spring 3
            float scatter2In = scatterDelay2.popSample() * scatterR;
            float junction2Out = spring2Out * scatterT + scatterState2 * scatterR;
            scatterState2 = spring2Out;

            // Process spring 3 with scatter input from junction 2 only (no direct input)
            spring3Out = spring3.processSample(junction2Out + scatter2In);
        }
        else
        {
            // High-stress mode: run 2-spring path to preserve realtime headroom at 96k/64.
            scatterState2 = spring2Out;
            scatterLpState2 = scatterLpAlpha * scatterLpState2;
        }

        // Feed back scattered energy through delay lines
        // Add frequency-tilted loss to tame metallic build-up (more HF loss)
        const float ringLoss = 0.45f;  // more loss in coupling rings to stop HF buildup
        scatterLpState1 = scatterLpAlpha * scatterLpState1 + (1.0f - scatterLpAlpha) * spring2Out;
        scatterLpState2 = scatterLpAlpha * scatterLpState2 + (1.0f - scatterLpAlpha) * spring3Out;
        scatterDelay1.pushSample(scatterLpState1 * scatterR * ringLoss);
        scatterDelay2.pushSample(scatterLpState2 * scatterR * ringLoss);

        // Early/late balance: spring 1 (33ms, bright attack) vs springs 2+3 (37ms+41ms, diffuse tail)
        // Allows shaping reverb character from tight/percussive (early) to lush/smooth (late)
        float springEarlyOut = spring1Out * springGain1;
        float lateTail = spring2Out * springGain2 + spring3Out * springGain3;
        if (performanceModeEnabled)
            lateTail += spring2Out * springGain3 * 0.6f;
        const float earlyWeight = earlyWeightValue;
        const float lateWeight = lateWeightValue;

        // Combine spring early reflections with ReflectionNetwork early reflections
        float combinedEarly = springEarlyOut + earlyRefOut * earlyReflectionMix;
        float output = combinedEarly * earlyWeight + lateTail * lateWeight;

        // Add classic noise floor (60Hz hum + broadband noise)
        if (noiseAmountSmooth > 0.0f)
            output += humGenerator.processSample() * noiseAmountSmooth;

        // Apply output gain and anti-aliased soft clip to prevent runaway feedback
        output *= outputGainSmooth;
        output = oversampledTanh.process(output);

        return output;
    }

private:
    float fs = 48000.0f;
    float currentModRate = 0.7f;  // Store modRate for passing to springs
    int controlUpdateIntervalSamples = 1;
    int controlSamplesUntilUpdate = 0;

    SingleSpring spring1;
    SingleSpring spring2;
    SingleSpring spring3;
    bool performanceModeEnabled = false;

    // Scattering junction delay lines (coupling rings between springs)
    InterpolatedDelayLine scatterDelay1{1024};
    InterpolatedDelayLine scatterDelay2{1024};
    float scatterState1 = 0.0f;
    float scatterState2 = 0.0f;
    float scatterLpState1 = 0.0f;
    float scatterLpState2 = 0.0f;
    float scatterLpAlpha = 0.0f;  // set in prepare
    float scatterT = 0.7f;        // Transmission coefficient
    float scatterR = 0.3f;        // Reflection coefficient (T + R = 1 for lossless)

    // Spring gain distribution (controlled by springBalance parameter)
    float springGain1 = 0.33f;
    float springGain2 = 0.34f;
    float springGain3 = 0.33f;

    // Input bandpass filter (200Hz HPF + 6kHz LPF)
    float inputHpAlpha = 0.97f;  // HPF coefficient
    float inputHpZ = 0.0f;       // HPF state
    float inputHpPrevIn = 0.0f;  // HPF previous input
    float inputLpAlpha = 0.9f;   // LPF coefficient
    float inputLpZ = 0.0f;       // LPF state

    // TA7200P driver saturation model
    SpringDriverSaturation driverSaturation;

    // 60Hz hum and noise floor generator
    SpringReverbHum humGenerator;

    // Early reflection network (Householder diffusion matrix)
    ReflectionNetwork earlyReflections;
    float earlyReflectionMix = 0.55f;  // Mix amount controlled by Reflections param

    // Modulated allpass diffusers (anti-metallic, modulated allpass pattern)
    // Two stages with different modulation rates for richer decorrelation
    ModulatedAllpass modAllpass1;
    ModulatedAllpass modAllpass2;

    // 2× oversampled waveshaper (anti-aliasing for output soft clip)
    OversampledTanh2x oversampledTanh;

    // Pre-delay line: delays the input before spring processing for spatial separation.
    InterpolatedDelayLine preDelayLine{9600};  // resized in prepare to 0-200ms at current SR
    float currentPreDelayMs = 0.0f;

    // Per-sample smoothing only for output-path values (gain, noise)
    // Structural params rely on block-rate smoothing in ReverbProcessor
    float outputGainSmooth = 1.0f;
    float noiseAmountSmooth = 0.0f;
    float smoothingCoeff = 0.002f;  // computed from time constant in prepare
    float earlyWeightValue = 1.0f;
    float lateWeightValue = 0.0f;
    float earlyWeightStep = 0.0f;
    float lateWeightStep = 0.0f;
    // Single threshold pair with no enter/exit hysteresis: the mode is latched
    // by configurePerformanceMode, so there is no per-callback flip-flop to
    // damp.
    static constexpr float kAdaptiveEnterSampleRate = 96000.0f;
    static constexpr int kAdaptiveEnterBlockSize = 64;

    Params currentParams;
};
