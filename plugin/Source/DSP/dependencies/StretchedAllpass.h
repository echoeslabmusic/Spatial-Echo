// StretchedAllpass: Stretched allpass cascade modelling spring dispersion/chirp. Stretched allpasses (delay > 1 sample)
//   reach the dispersion of 100s of unit-delay stages with 3-4x fewer stages; delay lengths grow exponentially through
//   the cascade so high frequencies arrive first (the spring chirp).
// Realtime-safety: No allocation in process()
// Based on: Parker 2011, "Efficient Dispersion Generation Structures for Spring Reverb Modeling"

#pragma once

#include "SchroederAllpass.h"
#include <array>
#include <cmath>

/**
 * Single stretched allpass filter
 * Unlike standard allpasses (1 sample delay), this uses multi-sample delays
 * to create equivalent dispersion with fewer stages
 *
 * H(z) = (g + z^-M) / (1 + g * z^-M)  where M > 1
 */
class StretchedAllpass
{
public:
    StretchedAllpass() = default;

    void prepare(float sampleRate) { delay.prepare(sampleRate, kDelayBufferSamples); }

    void reset() { delay.reset(); }

    /**
     * Set stretched allpass parameters
     * @param delaySamples delay length (typically 3-20 samples)
     * @param feedback allpass feedback coefficient (-0.7 to 0.7 typical)
     */
    void setParams(float delaySamples, float feedback)
    {
        delay.setDelay(std::max(1.0f, delaySamples));
        g = std::max(-0.95f, std::min(0.95f, feedback));
    }

    float processSample(float x)
    {
        float delayOut = delay.popSample();
        float v = x + g * delayOut;
        delay.pushSample(v);
        return delayOut - g * v;
    }

private:
    // StretchedAllpassCascade caps every stage delay at fs/600, which is 320
    // samples at the 192 kHz ceiling. 512 is the next power of two above that.
    // At 4096 this would be 36 stages x 3 springs x 2 morph processors x 16 KiB
    // = 3.5 MB of storage for a worst case that needs 442 KB.
    static constexpr int kDelayBufferSamples = 512;

    InterpolatedDelayLine delay{kDelayBufferSamples};
    float g = 0.5f;
};

/**
 * Stretched Allpass Cascade for spring reverb dispersion
 *
 * Uses Parker's efficient structure:
 * 1. Exponentially increasing delay lengths per stage
 * 2. Alternating feedback signs for rich spectral content
 *
 * This achieves the same dispersion as 100+ unit-delay allpasses
 * with only 16-36 stretched allpasses
 */
template <size_t NumStages = 16>
class StretchedAllpassCascade
{
public:
    StretchedAllpassCascade() = default;

    void prepare(float sampleRate)
    {
        fs = sampleRate;
        for (auto& ap : stages)
            ap.prepare(sampleRate);
    }

    void reset()
    {
        for (auto& ap : stages)
            ap.reset();
        for (auto& a : avgState)
            a = 0.0f;
    }

    /**
     * Set dispersion parameters
     * @param baseDelay starting delay in samples (3-8 typical)
     * @param stretch exponential stretch factor (1.1 to 1.4)
     * @param springSize 0-1 controls overall dispersion amount
     *
     * Stage feedback magnitude is fixed at the golden-ratio coefficient
     * (PocketVerbs, NonlinearSpace): ~62% of max diffusion, well below
     * instability, scaled by springSize and alternating in sign.
     */
    void setParams(float baseDelay, float stretch = 1.2f, float springSize = 0.5f)
    {
        // Scale parameters by spring size
        float scaledBase = baseDelay * (0.5f + springSize);
        static constexpr float kGoldenRatio = 0.6180339887f;

        // Cap maximum delay to keep all stages in audible chirp range.
        // Evidence: Stavely FV-1 keeps all 41 chirp APs in 5-8 samples (GDV 2-3.3kHz).
        //           Worsham found 12-20 samples optimal at 32kHz (GDV 0.8-1.4kHz).
        //           Below ~300 Hz GDV, stages add body but no perceptible chirp character.
        // fs/600 = ~80 samples at 48kHz → GDV floor of ~300 Hz.
        float maxDelay = fs / 600.0f;

        // Set each stage with exponentially increasing delay
        // and alternating feedback sign
        for (size_t i = 0; i < NumStages; ++i)
        {
            // Exponential delay progression
            float delayMult = std::pow(stretch, static_cast<float>(i));
            // Alternating feedback signs per Parker 2011 - richer spectral content
            // and better approximation of real spring dispersive phase response.
            float sign = (i % 2 == 0) ? -1.0f : 1.0f;
            float stageFeedback = sign * kGoldenRatio * (0.9f + 0.2f * springSize);

            // Small variation per stage for less periodic response. A
            // deterministic per-stage hash scatter measures the same late-tail
            // spectral smoothness (10.67 vs 10.69 dB) while shifting the
            // emergent preset T60s, so the simple pattern is used.
            // Tail texture is dominated by the FDN8+scatter network, not this
            // delay pattern.
            float variation = 1.0f + 0.03f * static_cast<float>(i % 4);

            // Cap delay at maxDelay to prevent sub-audible chirp waste
            float delay = std::min(scaledBase * delayMult * variation, maxDelay);
            stages[i].setParams(delay, stageFeedback);
        }
    }

    /**
     * Process sample through cascade
     * A 2-sample moving average is applied every 5th stage (MV2-style),
     * 7 instances across the 36 stages.
     */
    float processSample(float x)
    {
        float y = x;
        for (size_t i = 0; i < NumStages; ++i)
        {
            y = stages[i].processSample(y);
            // 2-sample average every 5th stage for HF diffusion
            if (i % 5 == 4)
            {
                float prev = avgState[i];
                avgState[i] = y;
                y = (y + prev) * 0.5f;
            }
        }
        return y;
    }

private:
    float fs = 48000.0f;
    std::array<StretchedAllpass, NumStages> stages;

    // 2-sample moving average state (active every 5th stage)
    std::array<float, NumStages> avgState{};
};
