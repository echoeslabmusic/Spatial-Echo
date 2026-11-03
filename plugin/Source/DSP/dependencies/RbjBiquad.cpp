// RbjBiquad: Second-order IIR filter with dynamic coefficient updating.
// Realtime-safety: No allocation/locks in process()

#include "RbjBiquad.h"

RbjBiquad::RbjBiquad()
{
}

RbjBiquad::~RbjBiquad()
{
}

void RbjBiquad::Init(BiquadType filterType, float fc, float fs, float Q, float gain_dB)
{
    // Save variables
    CurrentType = filterType;
    CutoffFrequency = fc;
    SampleRate = fs;
    QualityFactor = Q;
    Gain_dB = gain_dB;

    // Calculate filter coefficients
    CalcFilter();
}

void RbjBiquad::SetFc(float fc)
{
    // Only recalculate if cutoff frequency changed
    if (!juce::exactlyEqual(fc, CutoffFrequency))
    {
        // Save cutoff frequency
        CutoffFrequency = fc;

        // Calculate filter coefficients
        CalcFilter();
    }
}

void RbjBiquad::Reset(float fs)
{
    // Reset filter state variables
    yminustwo = yminusone = xminustwo = xminusone = 0.0;

    // Only recalculate if sample rate changed
    if (!juce::exactlyEqual(fs, SampleRate))
    {
        // Save sample rate
        SampleRate = fs;

        // Calculate filter coefficients
        CalcFilter();
    }
}

float RbjBiquad::ProcessSample(float xn)
{
    // Coefficients are normalised by a0 inside CalcFilter(), so a0 == 1 here.
    // Direct Form I, accumulated in double (see the note on the state members).
    const double x = static_cast<double>(xn);
    const double yn = b0 * x + b1 * xminusone + b2 * xminustwo - a1 * yminusone - a2 * yminustwo;

    // Update input state variables
    xminustwo = xminusone;
    xminusone = x;

    // Update output state variables
    yminustwo = yminusone;
    yminusone = yn;

    // Return current output sample
    return static_cast<float>(yn);
}
