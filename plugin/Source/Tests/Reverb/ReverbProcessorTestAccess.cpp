// ReverbProcessorTestAccess: Test-only access to ReverbProcessor internals (see the header).

#include "ReverbProcessorTestAccess.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include <algorithm>
#include <cmath>

bool ReverbProcessorTestAccess::stereoizerDelayBounds(float sampleRateHz)
{
    constexpr std::array<std::array<float, 2>, 4> delayAndDepthMs{
        {{{7.0f, 0.5f}}, {{11.0f, 0.4f}}, {{5.0f, 0.6f}}, {{13.0f, 0.3f}}}};
    for (const auto& config : delayAndDepthMs)
    {
        ReverbProcessor::StereoizerAllpass probe;
        probe.prepare(sampleRateHz, config[0], config[1], 0.7f, 0.45f);
        const float minimumDelay = static_cast<float>(probe.baseDelay) - probe.modDepthSamples;
        const float maximumDelay = static_cast<float>(probe.baseDelay) + probe.modDepthSamples;
        // Floor is 2.0, not 1.0: the stereoizer's allpass reader biases its
        // fractional part into [0.5, 1.5) and so reads one sample further back
        // than the requested delay.
        if (minimumDelay < 2.0f || maximumDelay > static_cast<float>(ReverbProcessor::StereoizerAllpass::kMaxDelay - 3))
            return false;
    }
    return true;
}

std::array<float, 4> ReverbProcessorTestAccess::engineParameterSnapshot(const ReverbProcessor& reverb,
                                                                        int engine) noexcept
{
    if (engine < 0 || engine > 1)
        return {};
    const auto& params = reverb.engineParams[static_cast<size_t>(engine)];
    return {params.size, params.decay, params.preDelay, params.width};
}

float ReverbProcessorTestAccess::worstWetFilterGainErrorDb(ReverbProcessor& reverb, float sampleRateHz)
{
    // A biquad whose recursion state runs in float32 loses its DC gain badly
    // at low cutoffs: perfect double coefficients with float32 state measure a
    // DC gain of 218 at 1 Hz. BiquadState keeps coefficients and state in
    // double (transposed Direct Form II), and this audit runs the actual
    // recursion, not H(e^jw) evaluated from the coefficients, so a precision
    // regression in that state shows up here.
    //
    // Cutoffs tested are the ones the reverb actually configures:
    //   classicWetHighPass       20 .. 600 Hz   (kWetLowCutMinHz .. Max)
    //   classicImpedanceRolloff  1000 .. 16000 Hz (kWetHighCutMinHz .. Max)
    //   classicTransducerResonance fixed 2500 Hz, +4 dB, Q 1.5
    using BiquadState = ReverbProcessor::BiquadState;
    reverb.sampleRate = sampleRateHz;
    float worstErrorDb = 0.0f;

    auto settledDcGain = [](BiquadState& filter, int samples) -> double
    {
        double last = 0.0;
        for (int n = 0; n < samples; ++n)
            last = filter.process(1.0f);
        return last;
    };

    auto sineGain = [&](BiquadState& filter, double freqHz) -> double
    {
        const int settle = static_cast<int>(sampleRateHz * 2.0f);
        const int measure = static_cast<int>(sampleRateHz * 2.0f);
        double inPower = 0.0, outPower = 0.0;
        for (int n = 0; n < settle + measure; ++n)
        {
            const double x = std::sin(2.0 * 3.14159265358979 * freqHz * n / sampleRateHz);
            const double y = filter.process(static_cast<float>(x));
            if (n >= settle)
            {
                inPower += x * x;
                outPower += y * y;
            }
        }
        return std::sqrt(outPower / std::max(1.0e-30, inPower));
    };

    auto note = [&](double measured, double expected)
    {
        const double errorDb = 20.0 * std::log10(std::max(1.0e-12, std::abs(measured)) / std::max(1.0e-12, expected));
        if (std::abs(errorDb) > std::abs(static_cast<double>(worstErrorDb)))
            worstErrorDb = static_cast<float>(errorDb);
    };

    // Lowpass: DC gain must be exactly 1.0. A float32 recursion state fails
    // this first.
    for (const float fc : {1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f})
    {
        if (fc > 0.45f * sampleRateHz)
            continue;
        BiquadState lp;
        reverb.configureLPF(lp, fc, true);
        note(settledDcGain(lp, 2000000), 1.0);
    }

    // Highpass: passband gain must be 1.0, and DC must be rejected.
    for (const float fc : {20.0f, 60.0f, 150.0f, 600.0f})
    {
        BiquadState hp;
        reverb.configureHPF(hp, fc, true);
        const double passband = sineGain(hp, std::min(4000.0f, 0.2f * sampleRateHz));
        note(passband, 1.0);

        BiquadState hpDc;
        reverb.configureHPF(hpDc, fc, true);
        const double dc = std::abs(settledDcGain(hpDc, 2000000));
        // A highpass that settles above -80 dBFS on constant input is leaking DC.
        if (dc > 1.0e-4)
            note(dc, 1.0e-4);
    }

    // Peaking EQ: +4 dB at 2500 Hz, Q 1.5.
    if (2500.0f < 0.45f * sampleRateHz)
    {
        BiquadState peak;
        reverb.configurePeakingEQ(peak, 2500.0f, 4.0f, 1.5f);
        note(sineGain(peak, 2500.0), std::pow(10.0, 4.0 / 20.0));
    }

    return worstErrorDb;
}

#endif
