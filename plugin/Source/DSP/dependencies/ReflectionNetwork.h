// ReflectionNetwork: Early-reflection generator for the spring tank: four recirculating delay lines mixed by a
//   Householder matrix, followed by a damping shelf.
// Realtime-safety: No allocation/locks in process() after prepare()

#pragma once

#include "SchroederAllpass.h"
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

/**
 * Shelf filter for frequency-dependent decay
 *
 * A float-precision RBJ high shelf scaled by lowGain. It is not the shared
 * Biquad: that class is double-precision Direct Form I with a different
 * coefficient derivation, so it is not sample-identical to this one.
 */
class ShelfFilter
{
public:
    void setParams(float lowGain, float highGain, float crossoverFreq, float sampleRate)
    {
        float w0 = 2.0f * juce::MathConstants<float>::pi * crossoverFreq / sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);

        float A = std::sqrt(highGain / lowGain);
        float alpha = sinw0 / 2.0f * std::sqrt((A + 1.0f / A) * 2.0f);

        float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha;
        b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha)) / a0;
        b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
        b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha)) / a0;
        a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha) / a0;

        // Apply low gain
        b0 *= lowGain;
        b1 *= lowGain;
        b2 *= lowGain;
    }

    void reset() { x1 = x2 = y1 = y2 = 0.0f; }

    float process(float x)
    {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
};

/**
 * Early reflection network using 4 parallel delay lines with Householder mixing
 * Creates dense, diffuse early reflections characteristic of spring reverb
 */
class ReflectionNetwork
{
public:
    ReflectionNetwork() = default;

    void prepare(float sampleRate, int /*samplesPerBlock*/)
    {
        fs = sampleRate;
        for (auto& d : delays)
            d.prepare(sampleRate, 65536);
        shelfFilter.reset();
    }

    void reset()
    {
        for (auto& d : delays)
            d.reset();
        for (auto& f : fbState)
            f = 0.0f;
        shelfFilter.reset();
    }

    void setParams(float reverbSize, float t60, float mix, float damping)
    {
        // Base delays in seconds - prime-ish ratios for inharmonic response
        constexpr float baseDelaysSec[4] = {0.07f, 0.17f, 0.23f, 0.29f};

        for (size_t i = 0; i < 4; ++i)
        {
            float delaySamples = baseDelaysSec[i] * reverbSize * fs;
            delays[i].setDelay(delaySamples);

            // T60-based feedback gain
            feedback[i] = std::pow(0.001f, delaySamples / (t60 * fs));
            feedback[i] *= 0.23f * mix * (0.735f + 0.235f * reverbSize);
        }

        // Damping shelf filter
        float dampDB = -1.0f - 9.0f * damping;
        float dampGain = std::pow(10.0f, dampDB / 20.0f);
        shelfFilter.setParams(1.0f, dampGain, 800.0f, fs);
    }

    float popSample()
    {
        float o0 = delays[0].popSample();
        float o1 = delays[1].popSample();
        float o2 = delays[2].popSample();
        float o3 = delays[3].popSample();

        // Householder matrix mixing: H = I - 2/N * ones
        // For N=4: each output = original - 0.5 * sum(all)
        constexpr float householderFactor = -2.0f / 4.0f;  // -0.5
        float sum = o0 + o1 + o2 + o3;
        float sumTimesHH = sum * householderFactor;

        o0 += sumTimesHH;
        o1 += sumTimesHH;
        o2 += sumTimesHH;
        o3 += sumTimesHH;

        // Store mixed outputs for feedback into delay lines
        fbState[0] = o0 * feedback[0];
        fbState[1] = o1 * feedback[1];
        fbState[2] = o2 * feedback[2];
        fbState[3] = o3 * feedback[3];

        // Weighted sum for output
        float result = fbState[0] + fbState[1] + fbState[2] + fbState[3];

        return shelfFilter.process(result * 0.25f);
    }

    void pushSample(float x)
    {
        // Feed back the Householder-mixed, attenuated outputs into the delay lines
        // combined with the new input - this creates true recirculating diffusion
        for (size_t i = 0; i < 4; ++i)
            delays[i].pushSample(x + fbState[i]);
    }

private:
    float fs = 48000.0f;
    std::array<InterpolatedDelayLine, 4> delays{InterpolatedDelayLine{65536}, InterpolatedDelayLine{65536},
                                                InterpolatedDelayLine{65536}, InterpolatedDelayLine{65536}};
    float feedback[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float fbState[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ShelfFilter shelfFilter;
};
