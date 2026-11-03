// RbjBiquad: Parametric second-order IIR filter supporting LPF, HPF, Notch, Peaking, and shelving modes.
// Realtime-safety: No allocation/locks in process()

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

enum class BiquadType
{
    LPF,
    HPF,
    Notch,
    Peaking,
    LowShelf,
    HighShelf
};

class RbjBiquad
{
public:
    // Ctor
    RbjBiquad();
    // Dtor
    ~RbjBiquad();

    // Initialise filter with specified parameters, call to change filter type
    // Only peaking and shelving filters require gain so set to 0.0f when NOT using those types.
    void Init(BiquadType filterType, float fc, float fs, float Q, float gain_dB);

    // Call to only set frequency cutoff
    void SetFc(float fc);

    // Reset filter
    void Reset(float fs);

    // Process a sample with the filter
    float ProcessSample(float xn);

private:
    // Coefficients and state are double, not float, and that is load-bearing.
    //
    // This is a Direct Form I biquad. At low cutoffs its poles crowd towards
    // z = 1 (fc = 40 Hz at 48 kHz puts them at radius 0.9963), and float32
    // cannot carry the small differences that determine the filter's DC
    // behaviour.
    //
    // Measured DC gain of this LPF design with float32 coefficients and state
    // (built standalone, unit step settled 200 s, cross-checked against H(z=1)
    // evaluated in double from the stored coefficients). It should be exactly
    // 1.0 at every cutoff:
    //
    //            fs = 44100        fs = 48000
    //   0.5 Hz   0.000000          0.000000     <- exactly silent
    //   1   Hz   0.000000          0.000000     <- exactly silent
    //   2   Hz   127.857986        2.035964
    //   3   Hz   1.000099          0.499717
    //   5   Hz   0.888678          1.138486
    //  10   Hz   1.029489          0.932247
    //  20   Hz   0.998154          1.014336
    //  50   Hz   1.000868          0.998244
    // 100   Hz   1.000305          0.999963
    //
    // In double the same design measures 1.000000 at every one of those
    // cutoffs and at both rates.
    //
    // Three separate float32 mechanisms cause those errors, and each is avoided:
    //  a) (1 - cos w) cancels to exactly zero in float32 for fc <= 1.865 Hz at
    //     48 kHz (1.713 Hz at 44.1 kHz), which zeroes every b coefficient. The
    //     filter is silent, not merely wrong. Half-angle identities cure this.
    //  b) Storing a1 (approx -2) and a2 (approx +1) as float32 destroys the
    //     denominator regardless of how the coefficients were derived: a0 + a1
    //     + a2 must resolve a quantity of order 1e-8 from a sum near 2, which
    //     is smaller than a float32 ULP there. A build with exact double
    //     derivation but float32 coefficient storage still measures 15.99 at
    //     fc = 1 Hz / 48 kHz, with the analytic DC gain outright infinite.
    //  c) The float32 state recursion adds a further error. With float32
    //     coefficients but double state, the step response lands
    //     exactly on the analytic H(1) (2.000000 at 44.1 kHz / 2 Hz) instead of
    //     127.86 - so float32 state alone accounts for the 64x discrepancy
    //     there.
    //
    // Hence both the coefficients and the four state variables are double, and
    // the derivation below is done in double with cancellation-free forms.
    // ProcessSample still takes and returns float, so callers are unaffected.
    //
    // Note for anyone re-measuring: FMA contraction changes the float32 step
    // response materially (-ffp-contract=off moves the 44.1 kHz / 2 Hz figure
    // from 127.86 to 1.04), and the 44.1 kHz and 48 kHz answers differ by two
    // orders of magnitude at the same cutoff. Optimisation level (-O0/-O2/-O3)
    // and FTZ do not matter. Always state the sample rate.

    // Feedback coeffs (denominator: a0 is normalised to 1 by CalcFilter)
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;

    // Feedforward coeffs (numerator: b0, b1, b2)
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;

    // State variables
    double xminusone = 0.0;
    double xminustwo = 0.0;
    double yminusone = 0.0;
    double yminustwo = 0.0;

    // Parameters
    float CutoffFrequency = 0.0f;
    float SampleRate = 0.0f;
    float QualityFactor = 0.0f;
    float Gain_dB = 0.0f;
    BiquadType CurrentType = BiquadType::LPF;

    // Calculate filter for current parameters.
    //
    // Coefficients follow the Robert Bristow-Johnson Audio EQ Cookbook, with
    // three deliberate deviations from a naive transcription:
    //
    //  1. All intermediates are computed in double, and the coefficients are
    //     stored in double (see the note on the members above).
    //  2. (1 - cos w) is evaluated as 2*sin^2(w/2) and (1 + cos w) as
    //     2*cos^2(w/2). Written literally, (1 - cos w) loses all significance
    //     for small w: in float32, cos(w) rounds to exactly 1.0 for w below
    //     2.4414e-4 (measured threshold fc <= 1.8650 Hz at 48 kHz, 1.7135 Hz at
    //     44.1 kHz), which makes every LPF b-coefficient exactly zero and the
    //     filter silent. Between roughly 2 Hz and 20 Hz the same cancellation
    //     leaves the LPF with a measured DC gain anywhere from 0.50 to 2.04
    //     instead of 1.0. The half-angle identities are exact in the same
    //     arithmetic and remove the cancellation entirely.
    //  3. The coefficients are normalised by a0 here rather than in
    //     ProcessSample. This is the standard placement; it is more accurate
    //     (one rounding instead of five per sample) and removes five divisions
    //     from every processed sample.
    //
    // Guards: a non-positive sample rate or Q, or a cutoff at/above Nyquist,
    // would produce NaN or an unstable filter (fc = 26 kHz at fs = 48 kHz
    // diverges to inf). Those inputs are clamped to the nearest usable value
    // instead of poisoning the signal path.
    void CalcFilter()
    {
        if (!(SampleRate > 0.0f))
            return;  // Nothing sensible to design against; keep prior coefficients.

        const double fs = static_cast<double>(SampleRate);

        // Keep the cutoff strictly inside (0, Nyquist). At or above Nyquist the
        // bilinear-transform poles land on/outside the unit circle.
        const double nyquistLimit = 0.4999 * fs;
        double fc = static_cast<double>(CutoffFrequency);
        if (!(fc > 0.0))
            fc = 0.0;
        else if (fc > nyquistLimit)
            fc = nyquistLimit;

        // Q of zero is a division by zero. Clamp rather than emit NaN.
        double Q = static_cast<double>(QualityFactor);
        if (!(Q > 1.0e-4))
            Q = 1.0e-4;

        // Omega: angular frequency, and its half-angle terms.
        const double w = 2.0 * juce::MathConstants<double>::pi * (fc / fs);
        const double halfW = 0.5 * w;
        const double sinHalf = std::sin(halfW);
        const double cosHalf = std::cos(halfW);

        const double cosw = std::cos(w);
        const double sinw = std::sin(w);

        // Cancellation-free forms of (1 - cos w) and (1 + cos w).
        const double oneMinusCos = 2.0 * sinHalf * sinHalf;
        const double onePlusCos = 2.0 * cosHalf * cosHalf;

        // Damping coefficient
        const double a = sinw / (2.0 * Q);

        // Gain for shelving and peaking filters
        const double A = std::pow(10.0, static_cast<double>(Gain_dB) / 40.0);

        // Variable for shelving filter coeff calculation (simplifies calculations)
        const double var2sqAa = 2.0 * std::sqrt(A) * a;

        double nb0 = 1.0, nb1 = 0.0, nb2 = 0.0;
        double na0 = 1.0, na1 = 0.0, na2 = 0.0;

        switch (CurrentType)
        {
            case BiquadType::LPF:

                nb0 = oneMinusCos / 2.0;
                nb1 = oneMinusCos;
                nb2 = oneMinusCos / 2.0;

                na0 = 1.0 + a;
                na1 = -2.0 * cosw;
                na2 = 1.0 - a;

                break;

            case BiquadType::HPF:

                nb0 = onePlusCos / 2.0;
                nb1 = -onePlusCos;
                nb2 = onePlusCos / 2.0;

                na0 = 1.0 + a;
                na1 = -2.0 * cosw;
                na2 = 1.0 - a;

                break;

            case BiquadType::Notch:

                nb0 = 1.0;
                nb1 = -2.0 * cosw;
                nb2 = 1.0;

                na0 = 1.0 + a;
                na1 = -2.0 * cosw;
                na2 = 1.0 - a;

                break;

            case BiquadType::Peaking:

                nb0 = 1.0 + a * A;
                nb1 = -2.0 * cosw;
                nb2 = 1.0 - a * A;

                na0 = 1.0 + (a / A);
                na1 = -2.0 * cosw;
                na2 = 1.0 - (a / A);

                break;

            case BiquadType::LowShelf:

                nb0 = A * ((A + 1.0) - (A - 1.0) * cosw + var2sqAa);
                nb1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
                nb2 = A * ((A + 1.0) - (A - 1.0) * cosw - var2sqAa);

                na0 = (A + 1.0) + (A - 1.0) * cosw + var2sqAa;
                na1 = (-2.0) * ((A - 1.0) + (A + 1.0) * cosw);
                na2 = (A + 1.0) + (A - 1.0) * cosw - var2sqAa;

                break;

            case BiquadType::HighShelf:

                nb0 = A * ((A + 1.0) + (A - 1.0) * cosw + var2sqAa);
                nb1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw);
                nb2 = A * ((A + 1.0) + (A - 1.0) * cosw - var2sqAa);

                na0 = (A + 1.0) - (A - 1.0) * cosw + var2sqAa;
                na1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw);
                na2 = (A + 1.0) - (A - 1.0) * cosw - var2sqAa;

                break;

            default:
                break;
        }

        if (!(std::abs(na0) > 0.0))
            return;  // Degenerate design; keep prior coefficients.

        const double inv = 1.0 / na0;

        b0 = nb0 * inv;
        b1 = nb1 * inv;
        b2 = nb2 * inv;
        a0 = 1.0;
        a1 = na1 * inv;
        a2 = na2 * inv;
    }
};
