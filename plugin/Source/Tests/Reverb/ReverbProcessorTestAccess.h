// ReverbProcessorTestAccess: Test-only access to ReverbProcessor internals (engine transition state, smoothing
//   constants, stereoizer bounds, wet recovery filter precision, drain fault injection).

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../DSP/Reverbprocessor.h"
#include <array>

struct ReverbProcessorTestAccess
{
    // Provenance for committed probe artifacts: the smoothing constants of the
    // exact build that produced them.
    static float structuralSmoothingMs() noexcept { return ReverbProcessor::kStructuralSmoothingMs; }
    static float controlSmoothingMs() noexcept { return ReverbProcessor::kControlSmoothingMs; }

    // True when every stereoizer stage keeps its modulated read inside its
    // delay buffer at this sample rate.
    static bool stereoizerDelayBounds(float sampleRateHz);

    static int drainingEngine(const ReverbProcessor& reverb) noexcept { return reverb.drainingEngine; }
    static int activeEngine(const ReverbProcessor& reverb) noexcept { return reverb.activeReverbEngine; }
    static bool engineTransitionActive(const ReverbProcessor& reverb) noexcept { return reverb.engineTransitionActive; }

    // {size, decay, preDelay, width} of one engine's smoothed parameters.
    static std::array<float, 4> engineParameterSnapshot(const ReverbProcessor& reverb, int engine) noexcept;

    // Time-domain precision audit of the wet recovery filters. Runs the real
    // BiquadState recursion (not an analytic evaluation of its coefficients)
    // at every cutoff the reverb configures, and returns the worst gain error
    // in dB. Sets the processor's sample rate to sampleRateHz.
    static float worstWetFilterGainErrorDb(ReverbProcessor& reverb, float sampleRateHz);

    // Poison the draining engine's next rendered sample with NaN once.
    static void injectDrainingEngineNonFiniteOnce(ReverbProcessor& reverb) noexcept
    {
        reverb.debugDrainNonFiniteOnce = true;
    }
};

#endif
