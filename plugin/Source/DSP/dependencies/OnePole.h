// OnePole: First-order IIR smoother with separate rise/fall (attack/release) time constants.
// Realtime-safety: No allocation/locks in process()

#pragma once

#include <cmath>

class OnePole
{
public:
    void setAlphaAsymmetric(float timeUp, float timeDown);  // Different attack/release, in seconds
    float processAsymmetric(float xn);                      // Uses separate up/down alphas
    void setCurrentValue(float value) { zminus1 = value; }

    // Clears the filter state and adopts a new sample rate.
    //
    // This deliberately does NOT clear AlphaUp/AlphaDown. With the
    // coefficients zeroed, processAsymmetric() degenerates to
    // y = zminus1 + 0 * (xn - zminus1), i.e. the output latches at 0 forever,
    // so a caller that set its time constants before calling reset() - or that
    // calls reset() again later, e.g. from a panic/flush path - would silently
    // get a dead filter. Reset clears state; it does not un-design the filter.
    //
    // The coefficients are sample-rate dependent (see setAlphaAsymmetric), so
    // a caller changing the sample rate must still re-apply its time constants.
    void reset(float sr)
    {
        zminus1 = 0.0f;
        SampleRate = sr;
    }

private:
    float AlphaUp = 0.0f;    // For increasing values (slower motor = longer delay)
    float AlphaDown = 0.0f;  // For decreasing values (faster motor = shorter delay)
    float zminus1 = 0.0f;
    float SampleRate = 0.0f;
};

// Set different time constants for increasing vs decreasing values
// timeUp = time constant when value is increasing (e.g., motor slowing down)
// timeDown = time constant when value is decreasing (e.g., motor speeding up)
// A time constant <= 0 or an unset sample rate gives an instant response
// (alpha = 1), the limit of 1 - exp(-1 / (time * sampleRate)).
inline void OnePole::setAlphaAsymmetric(float timeUp, float timeDown)
{
    if (!(SampleRate > 0.0f))
    {
        AlphaUp = AlphaDown = 1.0f;
        return;
    }
    AlphaUp = (timeUp > 0.0f) ? 1.0f - static_cast<float>(std::exp(-(1.0f / (timeUp * SampleRate)))) : 1.0f;
    AlphaDown = (timeDown > 0.0f) ? 1.0f - static_cast<float>(std::exp(-(1.0f / (timeDown * SampleRate)))) : 1.0f;
}

// Process with asymmetric attack/release - useful for motor simulation
inline float OnePole::processAsymmetric(float xn)
{
    // Choose alpha based on whether input is above or below current state
    float alpha = (xn > zminus1) ? AlphaUp : AlphaDown;
    float yn = zminus1 + alpha * (xn - zminus1);
    zminus1 = yn;
    return yn;
}
