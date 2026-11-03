// TapeNoiseProcessor: Tape noise generation with hiss, asperity, and dropouts.
// Realtime-safety: No allocation/locks in process()

#pragma once

#include <cmath>
#include <algorithm>
#include <cstdint>
#include "PCG32.h"

// Every generator below is per-sample i.i.d. noise or a per-sample Bernoulli
// impulse feeding a fixed-Hz filter. The filtered RMS of such a source falls as
// 1/sqrt(fs), so without compensation the whole tape noise bed (calibrated to
// -80..-67 dBFS at 48 kHz) would sit ~3 dB lower at 96 kHz and ~6 dB lower at
// 192 kHz. These helpers normalize that out and are exactly 1.0 at 48 kHz, the
// calibration rate.
namespace TapeNoiseRate
{
inline float whiteScale(float sampleRate) noexcept
{
    return (sampleRate > 0.0f) ? std::sqrt(sampleRate / 48000.0f) : 1.0f;
}

// Per-sample event probabilities encode a rate in events per second.
inline float eventScale(float sampleRate) noexcept
{
    return (sampleRate > 0.0f) ? (48000.0f / sampleRate) : 1.0f;
}

// A single-sample impulse acts on a fixed-Hz filter through its area, which
// is amplitude/fs, so holding the filtered response constant needs the
// amplitude to rise with fs while the impulse rate falls.
inline float impulseAmplitudeScale(float sampleRate) noexcept
{
    return (sampleRate > 0.0f) ? (sampleRate / 48000.0f) : 1.0f;
}

// Linear-form one-pole coefficient that preserves a time constant fixed at
// 48 kHz by the literal `coeffAt48k`.
inline float smoothingCoeff(float coeffAt48k, float sampleRate) noexcept
{
    return (sampleRate > 0.0f) ? (coeffAt48k * 48000.0f / sampleRate) : coeffAt48k;
}
}  // namespace TapeNoiseRate

/**
 * TapeNoiseProcessor - tape noise model
 *
 * Design: This processor generates tape hiss with spectral shaping.
 *
 * Features:
 * - Filtered white noise (HP ~2kHz, LP ~12kHz) for tape-like character (HISS)
 * - Signal-dependent asperity noise with envelope follower (ASPERITY)
 * - Random amplitude dropouts for worn tape simulation (DROPOUTS)
 * - Condition parameter affects noise level and brightness
 * - Intensity scaling for feedback loop accumulation
 * - Separate gain taps for loop, output, and reverb injection
 *
 * Uses standard noise generation and envelope-following techniques
 * with additions for asperity noise and dropout simulation.
 */

class EnvelopeFollower
{
public:
    EnvelopeFollower() = default;

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;
        envelope = 0.0f;
        setParameters(10.0f, 100.0f);  // Default: 10ms attack, 100ms release
    }

    /**
     * Set envelope parameters
     * @param attackMs - Attack time in milliseconds
     * @param releaseMs - Release time in milliseconds
     */
    void setParameters(float attackMs, float releaseMs)
    {
        // Calculate coefficients: alpha = 1 - exp(-1 / (time_sec * sampleRate))
        attackCoeff = 1.0f - std::exp(-1.0f / (attackMs * 0.001f * SampleRate));
        releaseCoeff = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * SampleRate));
    }

    /**
     * Process one sample
     * @param input - Input sample (absolute value taken internally)
     * @return Current envelope level (0-1 range approximately)
     */
    float process(float input)
    {
        float absInput = std::abs(input);
        float coeff = (absInput > envelope) ? attackCoeff : releaseCoeff;
        envelope = envelope + coeff * (absInput - envelope);
        return envelope;
    }

    float getEnvelope() const { return envelope; }

private:
    float SampleRate = 44100.0f;
    float envelope = 0.0f;
    float attackCoeff = 0.01f;
    float releaseCoeff = 0.001f;
};

class DropoutGenerator
{
public:
    DropoutGenerator() { rng.seedFromRandomDevice(); }

    void seed(uint32_t seed) { rng.seed(static_cast<uint64_t>(seed) ^ 0xD1B54A32D192ED03ULL, 0xDA3E39CB94B95BDBULL); }

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;
        dropoutGain = 1.0f;
        targetGain = 1.0f;
        samplesRemaining = 0;

        // Smooth gain transitions (~5ms)
        gainSmoothCoeff = 1.0f - std::exp(-1.0f / (0.005f * sampleRate));
    }

    /**
     * Set dropout parameters
     * @param probability - Probability per second (0-0.05 typical)
     * @param minDepth - Minimum dropout depth (0.3-0.7 typical)
     * @param maxDepth - Maximum dropout depth (0.5-1.0 typical)
     * @param minDurationMs - Minimum duration (1-10ms typical)
     * @param maxDurationMs - Maximum duration (10-50ms typical)
     */
    void setParameters(float probability, float minDepthValue, float maxDepthValue, float minDurationMs,
                       float maxDurationMs)
    {
        // Convert probability per second to probability per sample
        probPerSample = probability / SampleRate;
        this->minDepth = minDepthValue;
        this->maxDepth = maxDepthValue;
        this->minDurationSamples = static_cast<int>(minDurationMs * 0.001f * SampleRate);
        this->maxDurationSamples = static_cast<int>(maxDurationMs * 0.001f * SampleRate);
    }

    /**
     * Process one sample
     * @return Gain multiplier (1.0 = no dropout, 0.3-0.7 = dropout active)
     */
    float process()
    {
        // If not in a dropout, check if we should start one
        if (samplesRemaining <= 0)
        {
            if (rng.nextFloat() < probPerSample)
            {
                // Start a dropout
                float depth = minDepth + rng.nextFloat() * (maxDepth - minDepth);
                targetGain = 1.0f - depth;  // depth 0.5 -> gain 0.5

                // Random duration
                int durationRange = maxDurationSamples - minDurationSamples;
                samplesRemaining =
                    minDurationSamples + static_cast<int>(rng.nextFloat() * static_cast<float>(durationRange));
            }
            else
            {
                targetGain = 1.0f;  // No dropout
            }
        }
        else
        {
            samplesRemaining--;
            if (samplesRemaining <= 0)
            {
                targetGain = 1.0f;  // End dropout
            }
        }

        // Smooth gain transition
        dropoutGain = dropoutGain + gainSmoothCoeff * (targetGain - dropoutGain);

        return dropoutGain;
    }

private:
    PCG32 rng;

    float SampleRate = 44100.0f;
    float dropoutGain = 1.0f;
    float targetGain = 1.0f;
    float gainSmoothCoeff = 0.01f;

    float probPerSample = 0.0f;
    float minDepth = 0.3f;
    float maxDepth = 0.7f;
    int minDurationSamples = 44;
    int maxDurationSamples = 2205;
    int samplesRemaining = 0;
};

class AsperityNoiseGenerator
{
public:
    AsperityNoiseGenerator() { rng.seedFromRandomDevice(); }

    void seed(uint32_t seed) { rng.seed(static_cast<uint64_t>(seed) ^ 0x8D58AC26E5E6F1A5ULL, 0x9E3779B97F4A7C15ULL); }

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;
        eventScale = TapeNoiseRate::eventScale(sampleRate);
        impulseScale = TapeNoiseRate::impulseAmplitudeScale(sampleRate);

        // Initialize bandpass filter for harsh asperity spectrum (1-4kHz)
        hpState = 0.0f;
        lpState = 0.0f;

        float hpFc = 1000.0f / sampleRate;
        hpAlpha = 1.0f - std::exp(-2.0f * 3.14159f * hpFc);

        float lpFc = 4000.0f / sampleRate;
        lpAlpha = 1.0f - std::exp(-2.0f * 3.14159f * lpFc);
    }

    /**
     * Set asperity parameters
     * @param amount - Asperity noise amount (0-1)
     * @param sparsity - How sparse the impulses are (0=dense, 1=sparse)
     */
    void setParameters(float amount, float sparsity)
    {
        Amount = amount;
        // Sparsity affects impulse probability: 0.5 (dense) to 0.02 (sparse)
        impulseProbability = 0.5f - 0.48f * sparsity;
    }

    /**
     * Process one sample
     * @param signalLevel - Current signal envelope level (0-1)
     * @return Asperity noise sample (signal-dependent)
     */
    float process(float signalLevel)
    {
        // Generate sparse impulses. The probability is a per-second rate, and
        // the impulse amplitude compensates for the fixed-Hz filters seeing a
        // shorter sample period, so both the impulse density in time and the
        // resulting level are sample-rate invariant.
        float impulse = 0.0f;
        if (rng.nextFloat() < impulseProbability * eventScale)
        {
            impulse = rng.nextSignedFloat() * impulseScale;
        }

        // Bandpass filter (1-4kHz for harsh asperity character)
        float hp = impulse - hpState;
        hpState = hpState + hpAlpha * (impulse - hpState);
        lpState = lpState + lpAlpha * (hp - lpState);

        // Modulate by signal level (asperity is signal-dependent)
        // More signal = more asperity noise
        float modulated = lpState * (0.2f + 0.8f * signalLevel);

        return modulated * Amount;
    }

private:
    PCG32 rng;

    float SampleRate = 44100.0f;
    float Amount = 0.0f;
    float impulseProbability = 0.1f;
    float eventScale = 1.0f;
    float impulseScale = 1.0f;

    // Bandpass filter state
    float hpState = 0.0f;
    float lpState = 0.0f;
    float hpAlpha = 0.1f;
    float lpAlpha = 0.5f;
};

class TapeNoiseProcessor
{
public:
    TapeNoiseProcessor()
    {
        rng.seedFromRandomDevice();
        outputRng[0].seedFromRandomDevice();
        outputRng[1].seedFromRandomDevice();
    }
    ~TapeNoiseProcessor() = default;

    void setDeterministicSeed(uint32_t seed)
    {
        rng.seed(static_cast<uint64_t>(seed) ^ 0xA5A5A5A5u, 0xBF58476D1CE4E5B9ULL);
        outputRng[0].seed(static_cast<uint64_t>(seed) ^ 0x6A09E667F3BCC909ULL, 0x94D049BB133111EBULL);
        outputRng[1].seed(static_cast<uint64_t>(seed) ^ 0xBB67AE8584CAA73BULL, 0xD1B54A32D192ED03ULL);
        dropoutGen.seed(seed ^ 0x3C6EF372u);
        asperityGen.seed(seed ^ 0xBB67AE85u);
    }

    void reset(float sampleRate)
    {
        SampleRate = sampleRate;
        whiteScale = TapeNoiseRate::whiteScale(sampleRate);
        gainSmoothCoeff = TapeNoiseRate::smoothingCoeff(0.001f, sampleRate);

        // Initialize hiss filter states
        hpState = 0.0f;
        lpState = 0.0f;
        outputHpState[0] = outputHpState[1] = 0.0f;
        outputLpState[0] = outputLpState[1] = 0.0f;

        // Initialize envelope follower
        envFollower.reset(sampleRate);

        // Initialize asperity noise
        asperityGen.reset(sampleRate);

        // Initialize dropout generator
        dropoutGen.reset(sampleRate);

        // Calculate filter coefficients
        updateFilterCoeffs();

        currentNoiseSample = 0.0f;
        currentHissSample = 0.0f;
        currentAsperitySample = 0.0f;
        currentOutputNoiseSample[0] = currentOutputNoiseSample[1] = 0.0f;
        currentDropoutGain = 1.0f;
        currentEnvelope = 0.0f;
        smoothLoopGain = 0.0f;
        smoothOutputGain = 0.0f;
        smoothReverbGain = 0.0f;
        TargetLoopGain = 0.0f;
        TargetOutputGain = 0.0f;
        TargetReverbGain = 0.0f;
    }

    /**
     * Update noise parameters (extended for asperity and dropouts)
     * @param enabled - Master noise on/off
     * @param amount - User-facing noise amount (0-1)
     * @param condition - Tape condition: 0=pristine, 1=worn (affects level + brightness)
     * @param intensity - Feedback intensity (0-1) - noise scales with feedback
     * @param asperityAmount - Asperity noise level (0-1), default scales with condition
     * @param dropoutProbability - Dropout probability per second (0-0.05)
     */
    void updateParameters(bool enabled, float amount, float condition, float intensity, float asperityAmount = -1.0f,
                          float dropoutProbability = -1.0f)
    {
        NoiseEnabled = enabled;
        NoiseAmount = amount;
        Condition = condition;
        Intensity = intensity;

        // Design: keep hiss clearly audible from NoiseAmount even at lower condition.
        // Condition should increase degradation texture, not gate hiss audibility.
        // Condition 0.0 = -80 dB, Condition 0.5 ~= -72.5 dB, Condition 1.0 = -67 dB.
        const float conditionShaped = std::pow(std::clamp(Condition, 0.0f, 1.0f), 0.8f);
        float baseNoiseDb = -80.0f + 13.0f * conditionShaped;
        float baseNoiseGain = std::pow(10.0f, baseNoiseDb / 20.0f);

        // OSCILLATION SEEDING: Noise scales aggressively with high intensity.
        // Per Valhalla DSP: "there has to be noise in there" for self-oscillation to work.
        // At intensity > 0.8, noise ramps up exponentially to seed oscillation buildup.
        // Below 0.8: gentle scaling (0.3 to 1.0)
        // Above 0.8: exponential ramp (1.0 to 4.0) to provide oscillation seed energy
        float intensityFactor;
        if (Intensity < 0.8f)
        {
            // Normal range: gentle scaling
            intensityFactor = 0.3f + 0.7f * (Intensity / 0.8f);
        }
        else
        {
            // Oscillation range: exponential ramp to seed buildup
            float oscRange = (Intensity - 0.8f) / 0.2f;           // 0 to 1 in oscillation zone
            intensityFactor = 1.0f + 3.0f * oscRange * oscRange;  // 1.0 to 4.0 (quadratic)
        }

        // Apply user amount control
        float effectiveGain = NoiseEnabled ? (baseNoiseGain * NoiseAmount) : 0.0f;
        float loopEffectiveGain = effectiveGain * intensityFactor;

        // Set gains for different injection points
        // Self-oscillation seeding: loop noise scales with intensity.
        // At normal intensity: reduced noise to prevent accumulation
        // At high intensity: higher loop gain to seed oscillation
        // The two regimes are joined with a smoothstep over Intensity 0.7-0.9 -
        // a hard switch at 0.8 would put a +4 dB step in the loop-noise target
        // (measured -85.9 -> -82.8 dBFS across one step).
        const float wornLoopLift = conditionShaped * NoiseAmount;
        const float normalScale = 0.5f + 0.3f * wornLoopLift;
        const float oscScale = 0.8f + 0.1f * wornLoopLift;
        const float blendT = std::clamp((Intensity - 0.7f) / 0.2f, 0.0f, 1.0f);
        const float blend = blendT * blendT * (3.0f - 2.0f * blendT);
        const float loopGainScale = normalScale + (oscScale - normalScale) * blend;
        TargetLoopGain = loopEffectiveGain * loopGainScale;
        // Output bed should track the user control directly (not just feedback intensity),
        // otherwise high hiss settings remain hard to hear.
        TargetOutputGain = effectiveGain * 1.0f;
        // Reverb noise should remain subtle and not scale with feedback intensity.
        constexpr float reverbNoiseScale = 0.05f;  // reverb-send bed sits 26 dB below the loop noise
        TargetReverbGain = effectiveGain * reverbNoiseScale;

        // Design: Worn tape is slightly brighter/noisier in the highs (HF tilt).
        HfTilt = 1.0f + 0.9f * conditionShaped;  // 1.0 to 1.9x HF boost for worn tape

        updateFilterCoeffs();

        // ================================================================
        // Asperity Noise (signal-dependent harsh noise)
        // Default: scales with condition if not explicitly set
        // ================================================================
        // At zero amount the gain taps perform the fade. Keep the last
        // auto-scaled texture alive under that fade instead of independently
        // hard-muting asperity before the loop/reverb gains reach silence.
        float effectiveAsperity =
            (asperityAmount < 0.0f)
                ? (NoiseAmount > 0.0f ? (0.10f + 0.65f * conditionShaped) * NoiseAmount : AsperityAmount)
                : asperityAmount;
        AsperityAmount = effectiveAsperity;

        // Asperity gets sparser when tape is more worn (more harsh impulses)
        float sparsity = 0.22f + 0.48f * (1.0f - conditionShaped);
        asperityGen.setParameters(effectiveAsperity * 0.5f, sparsity);

        // Envelope follower: faster attack/release for worn tape
        float attackMs = 9.0f - 5.5f * conditionShaped;
        float releaseMs = 95.0f - 60.0f * conditionShaped;
        envFollower.setParameters(attackMs, releaseMs);

        // ================================================================
        // Dropouts (random amplitude dips)
        // Default: scales with condition if not explicitly set
        // ================================================================
        float effectiveDropout = (dropoutProbability < 0.0f) ? std::pow(conditionShaped, 1.35f) * 0.05f :  // 0% -> 5%
                                     dropoutProbability;
        DropoutProbability = effectiveDropout;

        // Dropout parameters: deeper and longer for more worn tape
        float minDepth = 0.28f + 0.34f * conditionShaped;       // 28-62% gain reduction
        float maxDepth = 0.50f + 0.42f * conditionShaped;       // 50-92% gain reduction
        float minDurationMs = 2.0f + 10.0f * conditionShaped;   // 2-12ms
        float maxDurationMs = 14.0f + 70.0f * conditionShaped;  // 14-84ms

        dropoutGen.setParameters(effectiveDropout, minDepth, maxDepth, minDurationMs, maxDurationMs);
    }

    /**
     * Process one sample with optional input for envelope follower
     * @param inputSample - Optional input for envelope follower (0 if not used)
     */
    void processSample(float inputSample = 0.0f)
    {
        // Update envelope follower with input signal
        float envelopeLevel = envFollower.process(inputSample);
        currentEnvelope = envelopeLevel;

        // Process dropout
        currentDropoutGain = dropoutGen.process();

        // Keep generating the same streams while their gains fade down. Do not
        // gate the samples to zero here: that would hard-mute all three noise
        // injection points before the smoothers can act.
        smoothLoopGain += (TargetLoopGain - smoothLoopGain) * gainSmoothCoeff;
        smoothOutputGain += (TargetOutputGain - smoothOutputGain) * gainSmoothCoeff;
        smoothReverbGain += (TargetReverbGain - smoothReverbGain) * gainSmoothCoeff;

        constexpr float silenceGain = 1.0e-9f;
        if ((!NoiseEnabled || NoiseAmount <= 0.0f) &&
            std::max({smoothLoopGain, smoothOutputGain, smoothReverbGain}) <= silenceGain)
        {
            smoothLoopGain = smoothOutputGain = smoothReverbGain = 0.0f;
            currentHissSample = 0.0f;
            currentAsperitySample = 0.0f;
            currentNoiseSample = 0.0f;
            currentOutputNoiseSample[0] = currentOutputNoiseSample[1] = 0.0f;
            return;
        }

        // ================================================================
        // Generate HISS (continuous filtered white noise)
        // ================================================================
        float white = rng.nextSignedFloat() * whiteScale;

        // High-pass filter (~2kHz) - removes low rumble
        float hp = white - hpState;
        hpState = hpState + hpAlpha * (white - hpState);

        // Low-pass filter (~12kHz) - removes harsh ultra-highs
        lpState = lpState + lpAlpha * (hp * HfTilt - lpState);

        currentHissSample = lpState;

        // ================================================================
        // Generate ASPERITY (signal-dependent harsh noise)
        // ================================================================
        currentAsperitySample = asperityGen.process(envelopeLevel);

        // Combine hiss and asperity
        currentNoiseSample = currentHissSample + currentAsperitySample;

        // Generate an independent output hiss bed for each stereo channel.
        for (int ch = 0; ch < 2; ++ch)
        {
            const float outputWhite = outputRng[ch].nextSignedFloat() * whiteScale;
            const float outputHp = outputWhite - outputHpState[ch];
            outputHpState[ch] += hpAlpha * (outputWhite - outputHpState[ch]);
            outputLpState[ch] += lpAlpha * (outputHp * HfTilt - outputLpState[ch]);
            currentOutputNoiseSample[ch] = outputLpState[ch];
        }
    }

    // Design: Loop noise accumulates in feedback (echoes get noisier with each repeat).
    float getLoopNoise() const { return currentNoiseSample * smoothLoopGain; }

    // Design: Output noise is a quieter background bed.
    float getOutputNoise() const { return currentNoiseSample * smoothOutputGain; }

    float getOutputNoiseForChannel(int channel) const
    {
        const int ch = (channel <= 0) ? 0 : 1;
        return currentOutputNoiseSample[ch] * smoothOutputGain;
    }

    // Design: Reverb noise adds "spring tank fizz" to reverb input.
    // Per-channel reverb bed. Uses the decorrelated per-channel hiss stream
    // (statistically identical to the loop stream, different PCG32 seed) plus
    // the shared signal-dependent asperity, so the level and character match
    // the loop bed while the two reverb inputs receive decorrelated signals. A correlated bed injects a phantom-centre component into the
    // tank; a diffuse one is what a spring/tape noise floor actually is.
    float getReverbNoiseForChannel(int channel) const
    {
        const int ch = (channel <= 0) ? 0 : 1;
        return (currentOutputNoiseSample[ch] + currentAsperitySample) * smoothReverbGain;
    }

    // Get dropout gain (1.0 = no dropout, <1.0 = dropout active)
    float getDropoutGain() const { return currentDropoutGain; }

    // Get individual noise components (for testing/metering)
    float getHissNoise() const { return currentHissSample * smoothLoopGain; }
    float getAsperityNoise() const { return currentAsperitySample * smoothLoopGain; }
    float getEnvelopeLevel() const { return currentEnvelope; }

    // Get raw shaped noise (for testing/metering)
    float getRawNoise() const { return currentNoiseSample; }

private:
    friend struct TapeNoiseProcessorTestAccess;

    void updateFilterCoeffs()
    {
        if (SampleRate <= 0.0f)
            return;

        // One-pole HP coefficient for ~2kHz cutoff
        float hpFc = 2000.0f / SampleRate;
        hpAlpha = 1.0f - std::exp(-2.0f * 3.14159f * hpFc);

        // One-pole LP coefficient for ~12kHz cutoff (affected by Condition)
        float lpFc = (10000.0f + 4000.0f * Condition) / SampleRate;  // 10-14kHz based on condition
        lpAlpha = 1.0f - std::exp(-2.0f * 3.14159f * lpFc);
    }

    // Random number generation
    PCG32 rng;
    PCG32 outputRng[2];

    // Sub-processors
    EnvelopeFollower envFollower;
    AsperityNoiseGenerator asperityGen;
    DropoutGenerator dropoutGen;

    // Parameters
    bool NoiseEnabled = false;
    float NoiseAmount = 0.12f;
    float Condition = 0.08f;
    float Intensity = 0.5f;
    float AsperityAmount = 0.0f;
    float DropoutProbability = 0.0f;
    float SampleRate = 44100.0f;

    // Hiss filter state
    float hpState = 0.0f;
    float lpState = 0.0f;
    float outputHpState[2] = {0.0f, 0.0f};
    float outputLpState[2] = {0.0f, 0.0f};
    float hpAlpha = 0.1f;
    float lpAlpha = 0.5f;
    float HfTilt = 1.0f;
    // Sample-rate normalizers (both exactly their 48 kHz literal at 48 kHz):
    // whiteScale holds the hiss/asperity level, gainSmoothCoeff holds the
    // ~21 ms gain-smoothing time constant that 0.001f gives at 48 kHz.
    float whiteScale = 1.0f;
    float gainSmoothCoeff = 0.001f;

    // Current noise samples
    float currentNoiseSample = 0.0f;
    float currentHissSample = 0.0f;
    float currentAsperitySample = 0.0f;
    float currentOutputNoiseSample[2] = {0.0f, 0.0f};
    float currentDropoutGain = 1.0f;
    float currentEnvelope = 0.0f;

    // Gain values for different injection points
    float TargetLoopGain = 0.0f;
    float TargetOutputGain = 0.0f;
    float TargetReverbGain = 0.0f;

    // Smoothed gains (avoid clicks)
    float smoothLoopGain = 0.0f;
    float smoothOutputGain = 0.0f;
    float smoothReverbGain = 0.0f;
};
