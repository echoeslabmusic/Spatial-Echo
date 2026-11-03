// ToneStackProcessor: Bass/Treble shelving EQ for tape echo path only.
// Realtime-safety: No allocation/locks in process()

#pragma once

#include "dependencies/wdftonestack.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>

/**
 * ToneStackProcessor - Bass/Treble EQ
 *
 * Design: This processor affects ONLY the echo/tape path.
 * Dry and reverb paths bypass the tone stack entirely.
 * See EchoesModel::ProcessBuffer for routing.
 */
class ToneStackProcessor
{
public:
    ToneStackProcessor() = default;
    ~ToneStackProcessor() = default;

    // Resets the tone stack processor with sample rate and number of channels.
    void Reset(float sampleRate, int numChannels);
    void UpdateParameters(float Bass, float Treble, float inputlevel);
    void ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize);
    // Auto-panic flush: zero WDF filter state in place so a NaN'd shelf cannot
    // keep re-poisoning the echo branch. No allocation - audio-thread safe.
    void clearAudioState();

private:
    // DSP filters (WDF-based shelving EQ)
    std::vector<std::unique_ptr<WDFToneStack>> WDFfilters;

    // Object variables
    int NumChannels = 2;
    float InputLevel = 0.0f;
    static constexpr double toneSmoothingTimeSeconds = 0.020;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedBassGain;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedTrebleGain;

    /**
     * Design: Map 0-1 knob position to cut/boost gain for tone stack
     * ±10 dB range (Bass @ 100Hz, Treble @ 5kHz)
     * "cut/boost control; no effect at 12 o'clock (straight up)"
     * - 0.0 (full CCW): -10 dB cut (gain = 0.3162)
     * - 0.5 (12 o'clock): Unity (gain = 1.0, no effect)
     * - 1.0 (full CW): +10 dB boost (gain = 3.162)
     *
     * Uses dB-linear mapping for perceptually correct audio pot behavior
     */
    double nonlinear_mapping(double x)
    {
        // Clamp input to 0-1 range
        x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);

        // Linear in dB: x=0 -> -10dB, x=0.5 -> 0dB, x=1 -> +10dB
        const double dB = (2.0 * x - 1.0) * 10.0;  // Maps 0-1 to -10..+10 dB
        return std::pow(10.0, dB / 20.0);          // Convert dB to linear gain
    }
};
