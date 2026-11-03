// HadamardFDN: Hadamard feedback-delay-network + absorption filtering for reverb diffusion.
// Realtime-safety: No allocation in process()
// Based on: Schlecht/Habets time-varying FDN and Chowdhury's implementations

#pragma once

#include "SchroederAllpass.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

/**
 * One-pole absorption filter for FDN feedback attenuation
 * Based on Jot & Chaigne (1991) - simpler and more stable than shelf design
 *
 * Design: Given DC gain (gDC) and Nyquist gain (gNY):
 *   r = gDC / gNY
 *   a1 = (1 - r) / (1 + r)
 *   b0 = (1 - a1) * gNY
 *   H(z) = b0 / (1 - a1 * z^-1)
 */
class FDNAbsorptionFilter
{
public:
    void reset() { z1 = 0.0f; }

    // The one-pole has no crossover parameter: its response is fixed by the DC
    // and Nyquist gains alone.
    void setGains(float dcGain, float nyquistGain)
    {
        // Clamp gains to ensure stability (must be < 1)
        float gDC = std::min(0.999f, std::max(0.001f, dcGain));
        float gNY = std::min(0.999f, std::max(0.001f, nyquistGain));

        // Jot (1991) one-pole design - simple and stable
        // H(z) = b0 / (1 - a1·z⁻¹)
        // H(1) = b0/(1-a1) = gDC,  H(-1) = b0/(1+a1) = gNY
        float r = gDC / gNY;
        a1 = (r - 1.0f) / (r + 1.0f);
        b0 = gDC * (1.0f - a1);
    }

    float process(float x)
    {
        // One-pole lowpass: y[n] = b0*x[n] + a1*y[n-1]
        float y = b0 * x + a1 * z1;
        z1 = y;
        return y;
    }

private:
    float b0 = 0.9f;
    float a1 = 0.1f;
    float z1 = 0.0f;
};

/**
 * 8-line Feedback Delay Network with Hadamard mixing
 * Hadamard matrix requires no multiplies (just additions/subtractions)
 */
class HadamardFDN8
{
public:
    static constexpr size_t N = 8;
    static constexpr float INV_SQRT_N = 0.35355339f;  // 1/sqrt(8)

    enum class ModulatedReadInterpolationMode
    {
        Cubic = 0,
        AllpassFast = 1
    };

    // Mutually prime delay lengths in samples at 48kHz (scaled by size)
    // Chosen for dense, non-periodic echo density
    static constexpr int baseDelays[N] = {
        2311, 2467, 2663, 2879,  // Longer delays
        503,  719,  997,  1361   // Shorter delays
    };

    HadamardFDN8() = default;

    void setModulatedReadInterpolationMode(ModulatedReadInterpolationMode mode)
    {
        modulatedReadInterpolationMode = mode;
    }

    void setPerformanceModeEnabled(bool enabled) { performanceModeEnabled = enabled; }

    void prepare(float sampleRate, int /*samplesPerBlock*/)
    {
        fs = sampleRate;
        sampleRateRatio = sampleRate / 48000.0f;
        allDelayBuffersPowerOfTwo = true;

        for (size_t i = 0; i < N; ++i)
        {
            delays[i].prepare(sampleRate, kDelayBufferSamples);
            if (!delays[i].hasPowerOfTwoBuffer())
                allDelayBuffersPowerOfTwo = false;
            absorptionFilters[i].reset();
        }

        modulationPhase = 0.0f;

        // DC blocker coefficient: target ~20 Hz cutoff regardless of sample rate
        dcBlockerCoeff = 1.0f - (juce::MathConstants<float>::twoPi * 20.0f / fs);

        reset();
    }

    void reset()
    {
        for (size_t i = 0; i < N; ++i)
        {
            delays[i].reset();
            delayOutputs[i] = 0.0f;
            dcPrevIn[i] = 0.0f;
            dcPrevOut[i] = 0.0f;
            lfoValueCurrent[i] = 0.0f;
            lfoValueStep[i] = 0.0f;
        }
        lfoControlCounter = 0;
        lfoNormCounter = 0;
        modulationPhase = 0.0f;
        rotationSin = 0.0f;
        rotationCos = 1.0f;
        rotationStepSin = 0.0f;
        rotationStepCos = 1.0f;
        rotationCValueCurrent = 1.0f;
        rotationCValueStep = 0.0f;
        rotationSValueCurrent = 0.0f;
        rotationSValueStep = 0.0f;
        rotationControlCounter = 0;
        rotationNormCounter = 0;
    }

    /**
     * Set FDN parameters
     * @param size 0-1: scales delay lengths (room size)
     * @param t60Low decay time for low frequencies
     * @param t60High decay time for high frequencies
     * @param modRate time-varying modulation rate in Hz (0.5-2 Hz typical)
     * @param modDepth modulation depth (0-1)
     */
    void setParams(float sizeValue, float t60Low, float t60High, float modRateValue = 0.7f, float modDepthValue = 0.15f)
    {
        this->size = std::max(0.1f, std::min(1.0f, sizeValue));
        this->modRate = modRateValue;
        this->modDepth = modDepthValue;
        modDepthSamples = modDepth * 16.0f * sampleRateRatio;
        allDelaysSafeForNoClamp = true;
        allDelaysSafeForNoClampAllpass = true;
        const float rotStepRad = juce::MathConstants<float>::twoPi * modRate / fs;
        rotationStepSin = std::sin(rotStepRad);
        rotationStepCos = std::cos(rotStepRad);
        rotationCValueCurrent = 1.0f;
        rotationCValueStep = 0.0f;
        rotationSValueCurrent = 0.0f;
        rotationSValueStep = 0.0f;
        rotationControlCounter = 0;

        // Per-line gain calculation (Jot 1992)
        // Each line needs its own gain based on its own delay length!
        // g_i = 10^(-3 * m_i / (T60 * fs))
        // This ensures all modes decay at the same rate regardless of delay length

        constexpr float delayScale = 1.0f;  // Use full base delay lengths for proper mode density
        for (size_t i = 0; i < N; ++i)
        {
            float delaySamples = static_cast<float>(baseDelays[i]) * delayScale * this->size * sampleRateRatio;
            delays[i].setDelay(delaySamples);
            baseDelaySamples[i] = delaySamples;

            const float minDelay = delaySamples - modDepthSamples;
            const float maxDelay = delaySamples + modDepthSamples;
            delaySafeForNoClamp[i] =
                (minDelay >= 4.0f) && (maxDelay <= static_cast<float>(delays[i].getMaxDelayForInterp()));
            // The allpass reader biases its fractional part into [0.5, 1.5),
            // so it reads one sample further back than the requested delay.
            delaySafeForNoClampAllpass[i] = (minDelay >= InterpolatedDelayLine::kMinAllpassDelay) &&
                                            (maxDelay <= static_cast<float>(delays[i].getMaxDelayForInterp() - 1));
            if (!delaySafeForNoClamp[i])
                allDelaysSafeForNoClamp = false;
            if (!delaySafeForNoClampAllpass[i])
                allDelaysSafeForNoClampAllpass = false;

            // Calculate per-line gains for this specific delay length
            float gL = std::pow(10.0f, -3.0f * delaySamples / (t60Low * fs));
            float gH = std::pow(10.0f, -3.0f * delaySamples / (t60High * fs));

            // Safety margin: trim gains to ensure stability.
            // Moderate attenuation - Jot formula already guarantees gains < 1 for finite T60.
            const float safety = 0.90f;  // ~0.9 dB loss per loop
            gL *= safety;
            gH *= safety;

            // Clamp to ensure stability (gains must be < 1)
            gL = std::min(0.95f, gL);
            gH = std::min(0.95f, gH);

            absorptionFilters[i].setGains(gL, gH);

            // LFO frequency spread (Schlecht 2017) - each delay gets slightly different rate
            // Spread factor: 0.7 to 1.3 (±30% spread for decorrelation)
            lfoFreqSpread[i] = 0.7f + 0.6f * (float)i / (float)(N - 1);
            const float phaseOffset = (i & 1) ? 0.25f : 0.0f;  // Quadrature for odd indices
            const float lfoPhase = modulationPhase * lfoFreqSpread[i] + phaseOffset;
            const float phaseRad = juce::MathConstants<float>::twoPi * lfoPhase;
            lfoOsc[i].sin = std::sin(phaseRad);
            lfoOsc[i].cos = std::cos(phaseRad);
            const float stepRad = juce::MathConstants<float>::twoPi * modRate * lfoFreqSpread[i] / fs;
            lfoOsc[i].stepSin = std::sin(stepRad);
            lfoOsc[i].stepCos = std::cos(stepRad);
            lfoValueCurrent[i] = lfoOsc[i].sin;
            lfoValueStep[i] = 0.0f;
        }
        lfoControlCounter = 0;
    }

    /**
     * Apply Hadamard-8 transform in-place
     * Uses fast Walsh-Hadamard Transform (only additions)
     */
    void hadamardTransform(float* x)
    {
        // Hand-unrolled 8-point FWHT avoids loop overhead in the FDN hot path.
        float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
        float x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7];

        const float s0 = x0 + x1;
        const float d0 = x0 - x1;
        const float s1 = x2 + x3;
        const float d1 = x2 - x3;
        const float s2 = x4 + x5;
        const float d2 = x4 - x5;
        const float s3 = x6 + x7;
        const float d3 = x6 - x7;

        const float q0 = s0 + s1;
        const float q1 = d0 + d1;
        const float q2 = s0 - s1;
        const float q3 = d0 - d1;
        const float q4 = s2 + s3;
        const float q5 = d2 + d3;
        const float q6 = s2 - s3;
        const float q7 = d2 - d3;

        x[0] = (q0 + q4) * INV_SQRT_N;
        x[1] = (q1 + q5) * INV_SQRT_N;
        x[2] = (q2 + q6) * INV_SQRT_N;
        x[3] = (q3 + q7) * INV_SQRT_N;
        x[4] = (q0 - q4) * INV_SQRT_N;
        x[5] = (q1 - q5) * INV_SQRT_N;
        x[6] = (q2 - q6) * INV_SQRT_N;
        x[7] = (q3 - q7) * INV_SQRT_N;
    }

    /**
     * Apply time-varying rotation to prevent metallic buildup
     * Based on Schlecht/Habets 2015
     */
    static constexpr int kRotationControlPeriod = 8;

    void updateRotationControl()
    {
        if (++rotationControlCounter < kRotationControlPeriod)
            return;
        rotationControlCounter = 0;
        const float invPeriod = 1.0f / static_cast<float>(kRotationControlPeriod);

        const float currentSin = rotationSin;
        const float newSin = rotationSin * rotationStepCos + rotationCos * rotationStepSin;
        const float newCos = rotationCos * rotationStepCos - rotationSin * rotationStepSin;
        rotationSin = newSin;
        rotationCos = newCos;

        if (++rotationNormCounter >= 1024)
        {
            const float mag = std::sqrt(rotationSin * rotationSin + rotationCos * rotationCos);
            if (mag > 0.0f)
            {
                const float invMag = 1.0f / mag;
                rotationSin *= invMag;
                rotationCos *= invMag;
            }
            rotationNormCounter = 0;
        }

        const float thetaCurrent = modDepth * 0.5f * currentSin;
        const float thetaNext = modDepth * 0.5f * rotationSin;
        const float cCurrent = std::cos(thetaCurrent);
        const float sCurrent = std::sin(thetaCurrent);
        const float cNext = std::cos(thetaNext);
        const float sNext = std::sin(thetaNext);
        rotationCValueCurrent = cCurrent;
        rotationCValueStep = (cNext - cCurrent) * invPeriod;
        rotationSValueCurrent = sCurrent;
        rotationSValueStep = (sNext - sCurrent) * invPeriod;
    }

    void applyTimeVaryingRotation(float* x)
    {
        if (modDepth <= 0.0f)
            return;

        // Update modulation phase
        modulationPhase += modRate / fs;
        if (modulationPhase > 1.0f)
            modulationPhase -= 1.0f;

        // Apply gentle Givens rotations to pairs of channels
        // This rotates eigenvalues without changing decay
        updateRotationControl();
        const float c = rotationCValueCurrent;
        const float s = rotationSValueCurrent;
        rotationCValueCurrent += rotationCValueStep;
        rotationSValueCurrent += rotationSValueStep;

        // Rotate pairs (0,1), (2,3), (4,5), (6,7)
        for (size_t i = 0; i < N; i += 2)
        {
            float a = x[i];
            float b = x[i + 1];
            x[i] = c * a - s * b;
            x[i + 1] = s * a + c * b;
        }
    }

    float processSample(float input)
    {
        // LFO phase is updated inside applyTimeVaryingRotation() - do not duplicate here
        const bool useMod = modDepth > 0.0f;
        if (useMod)
            updateLfoControl();

        // Read from all delay lines with LFO-modulated delay times
        // Each delay gets: different frequency (spread) + alternating quadrature phase
        if (useMod)
        {
            const bool useFastAllpass = performanceModeEnabled &&
                                        (modulatedReadInterpolationMode == ModulatedReadInterpolationMode::AllpassFast);
            const bool allSafe = useFastAllpass ? allDelaysSafeForNoClampAllpass : allDelaysSafeForNoClamp;
            if (allSafe)
            {
                for (size_t i = 0; i < N; ++i)
                {
                    const float lfoValue = lfoValueCurrent[i];
                    const float delaySamples = baseDelaySamples[i] + lfoValue * modDepthSamples;
                    delayOutputs[i] = useFastAllpass ? delays[i].popSampleAtNoClampAllpass(delaySamples)
                                                     : delays[i].popSampleAtNoClamp(delaySamples);
                    lfoValueCurrent[i] += lfoValueStep[i];
                }
            }
            else
            {
                for (size_t i = 0; i < N; ++i)
                {
                    const float lfoValue = lfoValueCurrent[i];
                    const float delaySamples = baseDelaySamples[i] + lfoValue * modDepthSamples;
                    if (useFastAllpass)
                    {
                        if (delaySafeForNoClampAllpass[i])
                            delayOutputs[i] = delays[i].popSampleAtNoClampAllpass(delaySamples);
                        else
                            delayOutputs[i] = delays[i].popSampleAtAllpass(delaySamples);
                    }
                    else
                    {
                        if (delaySafeForNoClamp[i])
                            delayOutputs[i] = delays[i].popSampleAtNoClamp(delaySamples);
                        else
                            delayOutputs[i] = delays[i].popSampleAt(delaySamples);
                    }
                    lfoValueCurrent[i] += lfoValueStep[i];
                }
            }
        }
        else
        {
            if (allDelaysSafeForNoClamp)
            {
                for (size_t i = 0; i < N; ++i)
                    delayOutputs[i] = delays[i].popSampleAtNoClamp(baseDelaySamples[i]);
            }
            else
            {
                for (size_t i = 0; i < N; ++i)
                {
                    if (delaySafeForNoClamp[i])
                        delayOutputs[i] = delays[i].popSampleAtNoClamp(baseDelaySamples[i]);
                    else
                        delayOutputs[i] = delays[i].popSampleAt(baseDelaySamples[i]);
                }
            }
        }

        if (useMod)
            renormalizeLfos();

        // Tap output BEFORE Hadamard (Jot 1992, JOS PASP)
        // Summing after Hadamard collapses to sqrt(N)*d[0] due to
        // the row-sum property, wasting 7 of 8 delay lines.
        // Normalize by 1/sqrt(N) for energy-preserving output.
        float output = 0.0f;
        for (size_t i = 0; i < N; ++i)
            output += delayOutputs[i];
        output *= INV_SQRT_N;

        // Apply Hadamard mixing (unitary, lossless) - for feedback path only
        hadamardTransform(delayOutputs);

        // Apply time-varying rotation (additional anti-metallic measure)
        applyTimeVaryingRotation(delayOutputs);

        // Process each delay line: add input, filter, write back
        // The input is added to every delay line, as in the reference FDN.
        processWriteStage(input);

        return output;
    }

private:
    struct LfoOsc
    {
        float sin = 0.0f;
        float cos = 1.0f;
        float stepSin = 0.0f;
        float stepCos = 1.0f;
    };

    static constexpr int kLfoControlPeriod = 8;

    void updateLfoControl()
    {
        if (++lfoControlCounter < kLfoControlPeriod)
            return;
        lfoControlCounter = 0;
        const float invPeriod = 1.0f / static_cast<float>(kLfoControlPeriod);
        for (size_t i = 0; i < N; ++i)
        {
            const float current = lfoOsc[i].sin;
            const float newSin = lfoOsc[i].sin * lfoOsc[i].stepCos + lfoOsc[i].cos * lfoOsc[i].stepSin;
            const float newCos = lfoOsc[i].cos * lfoOsc[i].stepCos - lfoOsc[i].sin * lfoOsc[i].stepSin;
            lfoOsc[i].sin = newSin;
            lfoOsc[i].cos = newCos;
            lfoValueCurrent[i] = current;
            lfoValueStep[i] = (newSin - current) * invPeriod;
        }
    }

    void renormalizeLfos()
    {
        if (++lfoNormCounter < 1024)
            return;
        for (size_t i = 0; i < N; ++i)
        {
            const float mag = std::sqrt(lfoOsc[i].sin * lfoOsc[i].sin + lfoOsc[i].cos * lfoOsc[i].cos);
            if (mag > 0.0f)
            {
                const float invMag = 1.0f / mag;
                lfoOsc[i].sin *= invMag;
                lfoOsc[i].cos *= invMag;
            }
        }
        lfoNormCounter = 0;
    }

    void processWriteStage(float input)
    {
        if (allDelayBuffersPowerOfTwo)
        {
            for (size_t i = 0; i < N; ++i)
            {
                const float feedback = absorptionFilters[i].process(delayOutputs[i] + input);
                // DC blocker: y = x - x_prev + R * y_prev
                float dc = feedback - dcPrevIn[i] + dcBlockerCoeff * dcPrevOut[i];
                dcPrevIn[i] = feedback;
                dcPrevOut[i] = dc;
                delays[i].pushSampleNoWrapCheck(dc);
            }
            return;
        }

        for (size_t i = 0; i < N; ++i)
        {
            const float feedback = absorptionFilters[i].process(delayOutputs[i] + input);
            // DC blocker: y = x - x_prev + R * y_prev
            float dc = feedback - dcPrevIn[i] + dcBlockerCoeff * dcPrevOut[i];
            dcPrevIn[i] = feedback;
            dcPrevOut[i] = dc;
            delays[i].pushSample(dc);
        }
    }

    float fs = 48000.0f;
    float sampleRateRatio = 1.0f;
    float size = 0.5f;
    float modRate = 0.7f;
    float modDepth = 0.15f;
    float modDepthSamples = 0.0f;
    float modulationPhase = 0.0f;
    float rotationSin = 0.0f;
    float rotationCos = 1.0f;
    float rotationStepSin = 0.0f;
    float rotationStepCos = 1.0f;
    float rotationCValueCurrent = 1.0f;
    float rotationCValueStep = 0.0f;
    float rotationSValueCurrent = 0.0f;
    float rotationSValueStep = 0.0f;
    int rotationControlCounter = 0;
    int rotationNormCounter = 0;

    // Longest line the FDN can ever ask for: baseDelays max (2879) * size max
    // (1.0) * sampleRateRatio max (192 kHz / 48 kHz = 4) = 11516 samples, plus
    // the modulation excursion (modDepth 1.0 * 16 * 4 = 64) and the 4-sample
    // interpolator guard. kDelayBufferSamples is the next power of two above
    // that, which both satisfies pushSampleNoWrapCheck and leaves headroom.
    //
    // At 131072 per line this would be 11x the worst case: 8 lines x
    // 3 springs x 2 morph processors x 128 KiB = 25.2 MB of Spring delay
    // storage per ReverbProcessor, and a 12.6 MB memcpy every time a Size
    // change triggers a morph (see WaveguideSpringReverbNetwork).
    static constexpr int kDelayBufferSamples = 16384;

    std::array<InterpolatedDelayLine, N> delays{
        InterpolatedDelayLine{kDelayBufferSamples}, InterpolatedDelayLine{kDelayBufferSamples},
        InterpolatedDelayLine{kDelayBufferSamples}, InterpolatedDelayLine{kDelayBufferSamples},
        InterpolatedDelayLine{kDelayBufferSamples}, InterpolatedDelayLine{kDelayBufferSamples},
        InterpolatedDelayLine{kDelayBufferSamples}, InterpolatedDelayLine{kDelayBufferSamples}};
    std::array<bool, N> delaySafeForNoClamp{false, false, false, false, false, false, false, false};
    std::array<bool, N> delaySafeForNoClampAllpass{false, false, false, false, false, false, false, false};
    bool allDelaysSafeForNoClamp = false;
    bool allDelaysSafeForNoClampAllpass = false;
    bool allDelayBuffersPowerOfTwo = false;
    bool performanceModeEnabled = false;
    ModulatedReadInterpolationMode modulatedReadInterpolationMode = ModulatedReadInterpolationMode::Cubic;

    std::array<FDNAbsorptionFilter, N> absorptionFilters;
    float delayOutputs[N] = {0};
    float baseDelaySamples[N] = {0};

    // DC blocker state for feedback path
    float dcPrevIn[N] = {0};
    float dcPrevOut[N] = {0};
    float dcBlockerCoeff = 0.9975f;
    float lfoFreqSpread[N] = {0.7f, 0.79f, 0.87f, 0.96f, 1.04f, 1.13f, 1.21f, 1.3f};  // ±30% spread
    LfoOsc lfoOsc[N]{};
    float lfoValueCurrent[N] = {0};
    float lfoValueStep[N] = {0};
    int lfoControlCounter = 0;
    int lfoNormCounter = 0;
};
