// WDFShelf1stOrder / WDFToneStack: First-order WDF shelving filters (complementary LP/HP split) for the bass/treble
//   tone stack: Bass @ 100Hz, Treble @ 5kHz, ±10dB range. Built on chowdsp_wdf.
// Realtime-safety: No allocation/locks in processSample(); setParams() rebuilds capacitor state.

#pragma once

#include "ThirdPartyIncludesBegin.h"
#include "chowdsp_wdf/chowdsp_wdf.h"
#include "ThirdPartyIncludesEnd.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

using namespace chowdsp::wdft;

/**
 * 1st-order WDF shelving filter using a complementary LP/HP split:
 * 1. One RC branch (not two independent states)
 * 2. hp = x - lp, so the split is exactly complementary
 * 3. The capacitor value is updated through setCapacitanceValue()
 * 4. The polarity-inverter pattern from the chowdsp_wdf examples
 */
template <typename Float>
class WDFShelf1stOrder
{
public:
    enum class Type
    {
        LowShelf,
        HighShelf
    };

    explicit WDFShelf1stOrder(Type t)
        : type(t)
    {
    }

    void prepare(double sampleRate)
    {
        fs = (Float)sampleRate;
        cap.prepare(fs);

        // Invalidate the cached RC design: the prewarped capacitance depends on
        // fs, so the next setParams() must recompute it even if fc is unchanged.
        designedForFs = (Float)0;
        designedForCutoff = (Float)0;
        designedForG = (Float)0;
    }

    // Zero the capacitor state without touching coefficients (RT-safe).
    void reset() { cap.reset(); }

    // fc in Hz, G is linear gain (e.g. 0.316..3.162 for +/-10dB)
    //
    // ---- Shelf-midpoint pole placement (boost/cut reciprocity) -------------
    //
    // The complementary split below realises
    //     low shelf:  H(s) = (s + G*p) / (s + p)      unity highs, G at DC
    //     high shelf: H(s) = (G*s + p) / (s + p)      unity lows,  G at Nyquist
    // where p is the RC pole 1/(R*C).
    //
    // Placing p at the requested corner frequency wc directly makes the two
    // knob directions cover different frequency ranges. For the low shelf the
    // boost transition then sits between wc and G*wc while the cut transition
    // sits between G*wc and wc, so a cut does not undo a boost of the same
    // size. Measured at 48 kHz on the 100 Hz bass shelf with the pole at wc:
    // full boost (G = 3.162) gives +7.4036 dB at 100 Hz but full cut
    // (G = 0.3162) only -2.5964 dB, a reciprocity error of +4.8073 dB at the
    // corner that stays above 1 dB from 20 Hz to 400 Hz. The 5 kHz treble
    // shelf behaves the same way.
    //
    // The standard cure (RBJ Audio EQ Cookbook shelving filters; Zavalishin,
    // "The Art of VA Filter Design", sec. on shelving filters) is to put the
    // shelf *midpoint* at wc instead of the pole: pole and zero straddle wc
    // geometrically, separated by a factor of G. That means
    //     low shelf:  p = wc / sqrt(G)   (zero lands at sqrt(G)*wc)
    //     high shelf: p = wc * sqrt(G)   (zero lands at wc / sqrt(G))
    // which gives |H(j*wc)| = sqrt(G) exactly - half the boost in dB - and
    // makes H_{1/G}(j*w) = 1 / H_G(j*w) at *every* frequency, not just at wc.
    //
    // Since p = 1/(R*C), that is a pure capacitance change:
    //     low shelf:  C = sqrt(G) / (R*wc)
    //     high shelf: C = 1 / (sqrt(G) * R*wc)
    //
    // Prewarping is deliberately still evaluated once at fc, not at the
    // G-dependent pole. Reciprocity is exact only if the boost and cut analog
    // prototypes share the same wc; prewarping each pole separately would warp
    // them by different amounts and reintroduce the asymmetry. Warping the
    // midpoint (the frequency the user actually aims at) is the accurate
    // choice, and it keeps std::tan() out of the per-sample path.
    //
    // Caching: fc and fs are constant for the life of a prepared filter, so
    // the tan() and the 1/(R*wc) reciprocal are computed once. G *does* change
    // per sample while the tone smoother runs, so the capacitance update is
    // keyed on G as well. That costs one sqrt plus one capacitance
    // propagation per changed sample; measured cost is in the header comment
    // of ToneStackProcessor's caller test.
    void setParams(Float fc, Float Glin)
    {
        // Clamp away from zero: G = 0 would make the low-shelf capacitance zero
        // and its wave impedance infinite. 1e-6 is -120 dB, far below the
        // +/-10 dB the knob can reach.
        G = std::min((Float)1.0e6, std::max((Float)1.0e-6, Glin));

        const Float newCutoff = std::max((Float)1, fc);
        if (!juce::exactlyEqual(newCutoff, designedForCutoff) || !juce::exactlyEqual(fs, designedForFs))
        {
            cutoff = newCutoff;
            designedForCutoff = newCutoff;
            designedForFs = fs;

            // Prewarp the analog corner so the bilinear transform puts the
            // shelf midpoint at exactly fc.
            const double wc =
                2.0 * (double)fs * std::tan(juce::MathConstants<double>::pi * (double)cutoff / (double)fs);
            invRwc = (Float)(1.0 / ((double)R * wc));

            designedForG = (Float)0;  // force the capacitance below to recompute
        }

        if (juce::exactlyEqual(G, designedForG))
            return;

        designedForG = G;

        const Float sqrtG = std::sqrt(G);
        const Float C = (type == Type::LowShelf) ? (sqrtG * invRwc) : (invRwc / sqrtG);

        // Update the WDF capacitor element
        cap.setCapacitanceValue(C);
    }

    inline Float processSample(Float x) noexcept
    {
        vs.setVoltage(x);

        // Match chowdsp_wdf RC example: series -> polarity invert -> voltage source
        vs.incident(inv.reflected());
        inv.incident(vs.reflected());

        // Low-pass taken across capacitor (series RC driven by ideal source)
        const Float lp = voltage<Float>(cap);

        // Guaranteed complementary split: hp + lp = x
        const Float hp = x - lp;

        if (type == Type::HighShelf)
            return lp + G * hp;  // unity lows, G highs
        else
            return hp + G * lp;  // unity highs, G lows
    }

private:
    Type type;
    Float fs = (Float)48000;
    Float cutoff = (Float)1000;
    Float G = (Float)1;

    // Cache keys for the RC design above. Zero means "not yet designed".
    Float designedForCutoff = (Float)0;
    Float designedForFs = (Float)0;
    Float designedForG = (Float)0;

    // 1 / (R * wc) with wc prewarped at fc. Depends only on fc and fs.
    Float invRwc = (Float)1.0e-9;

    static constexpr Float R = (Float)100000;

    ResistorT<Float> res{R};
    CapacitorT<Float> cap{(Float)1e-9f, (Float)48000};  // placeholder; overwritten by setParams()

    WDFSeriesT<Float, decltype(res), decltype(cap)> series{res, cap};
    PolarityInverterT<Float, decltype(series)> inv{series};
    IdealVoltageSourceT<Float, decltype(inv)> vs{inv};
};

/**
 * Tone Stack: Bass shelf @ 100Hz + Treble shelf @ 5kHz
 * ±10dB range, unity at 12 o'clock
 */
class WDFToneStack
{
public:
    WDFToneStack() = default;
    ~WDFToneStack() = default;

    void prepare(double sampleRate)
    {
        bass.prepare(sampleRate);
        treble.prepare(sampleRate);
    }

    // Zero both shelves' capacitor state (RT-safe; coefficients untouched).
    void reset()
    {
        bass.reset();
        treble.reset();
    }

    // Design: Bass @ 100Hz, Treble @ 5kHz
    // Parameters are linear gain values (e.g. 0.316 to 3.162 for ±10dB)
    void updateParams(double bassG, double trebleG)
    {
        bass.setParams(100.0f, (float)bassG);       // Bass shelf at 100 Hz
        treble.setParams(5000.0f, (float)trebleG);  // Treble shelf at 5 kHz
    }

    inline double processSample(double x)
    {
        float y = bass.processSample((float)x);
        y = treble.processSample(y);
        return y;
    }

private:
    WDFShelf1stOrder<float> bass{WDFShelf1stOrder<float>::Type::LowShelf};
    WDFShelf1stOrder<float> treble{WDFShelf1stOrder<float>::Type::HighShelf};
};
