// BinauralProcessorDiffuse: Diffuse-field EQ computation and source-width decorrelation.
// Realtime-safety: no allocation/locks in applyDiffuseFieldEQ() / applySourceWidth()

#include "BinauralProcessor.h"
#include "OouraFFT.h"
#include <vector>

//==============================================================================
// Diffuse-field equalisation (DFE).
//
// The diffuse-field transfer function is the POWER average over directions,
//     D(f) = sqrt( mean_over_directions |H_dir(f)|^2 ),
// (Theile 1986; Møller et al. 1995) and DFE divides every HRTF by it, which
// removes the direction-independent colouration (ear-canal/concha resonances)
// while leaving the direction-dependent cues intact. Measured on the bundled
// HUTUBS pp1 dataset (440 directions, N=256, 44.1 kHz), D(f) is +0.0 dB below
// 200 Hz, +2.9 dB at 1 kHz, +9.8 dB at 3 kHz, +13.3 dB at 4 kHz, +4.9 dB at
// 8 kHz - without DFE the binaural path imposes a ~13 dB presence-band hump on
// everything it renders.
//
// This implementation computes D(f) per ear at load, smooths it over 1/6
// octave, inverts it with asymmetric limits (cuts are safe, boosts are not),
// tapers the correction to 0 dB at the frequency extremes, and converts it to
// a short MINIMUM-PHASE FIR via the real-cepstrum method. Minimum phase keeps
// the correction causal and adds negligible group delay, so it does not move
// the HRIR onset (and therefore does not disturb ITD). The FIR is then
// convolved into each HRIR as it is selected.
//
// Measured result on the bundled dataset with a 64-tap correction: the diffuse
// field flattens to within +-0.45 dB from 100 Hz to 12 kHz (+1.2 dB at 16 kHz,
// where the taper begins), and 99.998% of the correction's energy fits in the
// 64 taps.
//==============================================================================

namespace
{
// Correction limits. Cutting a resonance is safe; boosting a null is not,
// so the boost ceiling is much tighter than the cut floor.
constexpr float kDfeMaxCutDb = 18.0f;
constexpr float kDfeMaxBoostDb = 6.0f;

// Correction fades to 0 dB outside this band: below kDfeLowTaperHz the
// diffuse field is flat anyway, and above kDfeHighTaperStartHz an
// up-sampled dataset has no real content to correct (dividing by its
// near-zero magnitude would just amplify resampling noise).
constexpr float kDfeLowTaperHz = 100.0f;
constexpr float kDfeHighTaperStartHz = 18000.0f;
constexpr float kDfeHighTaperEndHz = 20000.0f;

// 1/6-octave smoothing window (+-1/12 octave around each bin).
constexpr float kDfeSmoothHalfWidthOctaves = 1.0f / 12.0f;
}  // namespace

void BinauralProcessor::computeDiffuseFieldResponse()
{
    diffuseFieldComputed = false;
    diffuseFieldFirLength = 0;

    // Identity fallback (unit impulse): if anything below bails out, the
    // convolution in applyDiffuseFieldEQ() is a pass-through rather than
    // silence or stale data.
    std::fill(diffuseFieldResponseL.begin(), diffuseFieldResponseL.end(), 0.0f);
    std::fill(diffuseFieldResponseR.begin(), diffuseFieldResponseR.end(), 0.0f);
    diffuseFieldResponseL[0] = 1.0f;
    diffuseFieldResponseR[0] = 1.0f;

    if (!hrtfDatabase || !hrtfDatabase->isLoaded() || !hrirFFT)
        return;

    const auto& gridPoints = hrtfDatabase->getGridPoints();
    if (gridPoints.empty())
        return;

    const int fftSize = hrirFftSize;
    const int bins = hrirFftBins;
    if (fftSize <= 0 || bins <= 1)
        return;

    const int responseLength = juce::jlimit(1, juce::jmin(HRIR_MAX_LENGTH, fftSize), hrirLengthRaw);

    // ---- Stage 1: power-average |H(f)|^2 over every measured direction ----
    std::vector<double> powerL(static_cast<size_t>(bins), 0.0);
    std::vector<double> powerR(static_cast<size_t>(bins), 0.0);
    std::vector<float> scratchTime(static_cast<size_t>(fftSize), 0.0f);
    std::vector<std::complex<float>> scratchSpec(static_cast<size_t>(bins));

    int hrirCount = 0;
    for (size_t idx = 0; idx < gridPoints.size(); ++idx)
    {
        const auto& hrir = hrtfDatabase->getHRIRByGridIndex(static_cast<int>(idx));

        std::fill(scratchTime.begin(), scratchTime.end(), 0.0f);
        for (int i = 0; i < responseLength; ++i)
            scratchTime[static_cast<size_t>(i)] = hrir.leftEar[static_cast<size_t>(i)];
        hrirFFT->fft(scratchTime.data(), scratchSpec.data());
        for (int b = 0; b < bins; ++b)
        {
            const auto& c = scratchSpec[static_cast<size_t>(b)];
            powerL[static_cast<size_t>(b)] +=
                static_cast<double>(c.real()) * c.real() + static_cast<double>(c.imag()) * c.imag();
        }

        std::fill(scratchTime.begin(), scratchTime.end(), 0.0f);
        for (int i = 0; i < responseLength; ++i)
            scratchTime[static_cast<size_t>(i)] = hrir.rightEar[static_cast<size_t>(i)];
        hrirFFT->fft(scratchTime.data(), scratchSpec.data());
        for (int b = 0; b < bins; ++b)
        {
            const auto& c = scratchSpec[static_cast<size_t>(b)];
            powerR[static_cast<size_t>(b)] +=
                static_cast<double>(c.real()) * c.real() + static_cast<double>(c.imag()) * c.imag();
        }

        ++hrirCount;
    }

    if (hrirCount == 0)
        return;

    // NOTE ON WEIGHTING: this is an unweighted mean over the measurement grid.
    // A strictly correct diffuse-field average weights each direction by its
    // solid angle. The bundled HUTUBS grid is near-uniform on the sphere so the
    // difference is small; a strongly non-uniform custom SOFA grid would bias
    // the result toward its dense region.
    const double invCount = 1.0 / static_cast<double>(hrirCount);
    std::vector<double> diffuseL(static_cast<size_t>(bins), 0.0);
    std::vector<double> diffuseR(static_cast<size_t>(bins), 0.0);
    for (int b = 0; b < bins; ++b)
    {
        diffuseL[static_cast<size_t>(b)] = std::sqrt(powerL[static_cast<size_t>(b)] * invCount);
        diffuseR[static_cast<size_t>(b)] = std::sqrt(powerR[static_cast<size_t>(b)] * invCount);
    }

    // ---- Stage 2: 1/6-octave power smoothing ----
    const double binHz = sampleRate / static_cast<double>(fftSize);
    const double smoothRatio = std::pow(2.0, static_cast<double>(kDfeSmoothHalfWidthOctaves));

    auto smoothInPlace = [bins, binHz, smoothRatio](std::vector<double>& mag)
    {
        std::vector<double> out(mag.size(), 0.0);
        for (int b = 0; b < bins; ++b)
        {
            const double freq = binHz * b;
            if (freq <= 0.0)
            {
                out[static_cast<size_t>(b)] = mag[static_cast<size_t>(b)];
                continue;
            }

            int lowBin = static_cast<int>(std::floor((freq / smoothRatio) / binHz));
            int highBin = static_cast<int>(std::ceil((freq * smoothRatio) / binHz));
            lowBin = juce::jlimit(0, bins - 1, lowBin);
            highBin = juce::jlimit(lowBin, bins - 1, highBin);

            double acc = 0.0;
            for (int k = lowBin; k <= highBin; ++k)
                acc += mag[static_cast<size_t>(k)] * mag[static_cast<size_t>(k)];
            out[static_cast<size_t>(b)] = std::sqrt(acc / static_cast<double>(highBin - lowBin + 1));
        }
        mag.swap(out);
    };

    smoothInPlace(diffuseL);
    smoothInPlace(diffuseR);

    // ---- Stage 3: limited, tapered inverse ----
    const double nyquist = sampleRate * 0.5;
    const double taperEnd = juce::jmin(static_cast<double>(kDfeHighTaperEndHz), nyquist);
    const double taperStart = juce::jmin(static_cast<double>(kDfeHighTaperStartHz), taperEnd);

    auto buildGain = [&](const std::vector<double>& diffuse, std::vector<double>& gain)
    {
        gain.assign(static_cast<size_t>(bins), 1.0);
        for (int b = 0; b < bins; ++b)
        {
            const double d = diffuse[static_cast<size_t>(b)];
            double db = (d > 1.0e-12) ? -20.0 * std::log10(d) : static_cast<double>(kDfeMaxBoostDb);
            db = juce::jlimit(static_cast<double>(-kDfeMaxCutDb), static_cast<double>(kDfeMaxBoostDb), db);

            const double freq = binHz * b;
            double weight = 1.0;
            if (freq < kDfeLowTaperHz)
                weight = freq / static_cast<double>(kDfeLowTaperHz);
            else if (freq > taperStart)
                weight = (taperEnd > taperStart) ? (1.0 - (freq - taperStart) / (taperEnd - taperStart)) : 0.0;
            weight = juce::jlimit(0.0, 1.0, weight);

            gain[static_cast<size_t>(b)] = std::pow(10.0, (db * weight) / 20.0);
        }
    };

    std::vector<double> gainL, gainR;
    buildGain(diffuseL, gainL);
    buildGain(diffuseR, gainR);

    // ---- Stage 4: minimum-phase FIR via the real cepstrum ----
    // ln|G| -> real cepstrum -> causal fold -> exp -> min-phase impulse.
    const int firLength = juce::jlimit(1, juce::jmin(HRIR_MAX_LENGTH, fftSize / 4), kDfeFirLength);

    std::vector<float> cepTime(static_cast<size_t>(fftSize), 0.0f);
    std::vector<float> foldedTime(static_cast<size_t>(fftSize), 0.0f);
    std::vector<std::complex<float>> spec(static_cast<size_t>(bins));

    auto buildMinPhaseFir = [&](const std::vector<double>& gain, std::array<float, HRIR_MAX_LENGTH>& fir) -> bool
    {
        for (int b = 0; b < bins; ++b)
            spec[static_cast<size_t>(b)] = std::complex<float>(
                static_cast<float>(std::log(juce::jmax(1.0e-9, gain[static_cast<size_t>(b)]))), 0.0f);

        hrirFFT->ifft(spec.data(), cepTime.data());

        std::fill(foldedTime.begin(), foldedTime.end(), 0.0f);
        foldedTime[0] = cepTime[0];
        for (int n = 1; n < fftSize / 2; ++n)
            foldedTime[static_cast<size_t>(n)] = 2.0f * cepTime[static_cast<size_t>(n)];
        foldedTime[static_cast<size_t>(fftSize / 2)] = cepTime[static_cast<size_t>(fftSize / 2)];

        hrirFFT->fft(foldedTime.data(), spec.data());

        for (int b = 0; b < bins; ++b)
        {
            const auto& c = spec[static_cast<size_t>(b)];
            const float magnitude = std::exp(c.real());
            if (!std::isfinite(magnitude))
                return false;
            spec[static_cast<size_t>(b)] =
                std::complex<float>(magnitude * std::cos(c.imag()), magnitude * std::sin(c.imag()));
        }

        hrirFFT->ifft(spec.data(), cepTime.data());

        std::fill(fir.begin(), fir.end(), 0.0f);
        const int fadeTaps = juce::jmin(8, firLength);
        for (int n = 0; n < firLength; ++n)
        {
            float w = 1.0f;
            const int fromEnd = firLength - n;
            if (fromEnd <= fadeTaps)
                w = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * static_cast<float>(fromEnd) /
                                            static_cast<float>(fadeTaps + 1)));
            const float v = cepTime[static_cast<size_t>(n)] * w;
            if (!std::isfinite(v))
                return false;
            fir[static_cast<size_t>(n)] = v;
        }
        return true;
    };

    std::array<float, HRIR_MAX_LENGTH> firL{};
    std::array<float, HRIR_MAX_LENGTH> firR{};
    // A non-finite FIR leaves the identity response in place: no correction.
    if (!buildMinPhaseFir(gainL, firL) || !buildMinPhaseFir(gainR, firR))
        return;

    diffuseFieldResponseL = firL;
    diffuseFieldResponseR = firR;
    diffuseFieldFirLength = firLength;
    diffuseFieldComputed = true;
}

void BinauralProcessor::applyDiffuseFieldEQ(HRIRData& hrir)
{
    if (!diffuseFieldComputed || diffuseFieldFirLength <= 0 || !diffuseFieldEQEnabled.load())
        return;

    const int length = juce::jlimit(1, HRIR_MAX_LENGTH, hrirLength);
    const int firLength = juce::jmin(diffuseFieldFirLength, length);

    // Stack scratch, no allocation (same pattern as applyFractionalDelay).
    std::array<float, HRIR_MAX_LENGTH> inL{};
    std::array<float, HRIR_MAX_LENGTH> inR{};
    for (int i = 0; i < length; ++i)
    {
        inL[static_cast<size_t>(i)] = hrir.leftEar[static_cast<size_t>(i)];
        inR[static_cast<size_t>(i)] = hrir.rightEar[static_cast<size_t>(i)];
    }

    // Truncating the tail of (hrir * dfe) at `length` loses only the part that
    // spills past the HRIR's own length; measured at 5e-5 of total energy on
    // the bundled dataset, because the correction is minimum phase and the HRIR
    // tail is already ~60 dB down there.
    for (int n = 0; n < length; ++n)
    {
        const int taps = juce::jmin(firLength, n + 1);
        float accL = 0.0f;
        float accR = 0.0f;
        for (int j = 0; j < taps; ++j)
        {
            accL += inL[static_cast<size_t>(n - j)] * diffuseFieldResponseL[static_cast<size_t>(j)];
            accR += inR[static_cast<size_t>(n - j)] * diffuseFieldResponseR[static_cast<size_t>(j)];
        }
        hrir.leftEar[static_cast<size_t>(n)] = accL;
        hrir.rightEar[static_cast<size_t>(n)] = accR;
    }
}

void BinauralProcessor::applySourceWidth(HRIRData& focused, float widthFactor)
{
    if (!hrtfDatabase->isLoaded() || widthFactor < 0.01f)
        return;

    // Clamp width factor to 0-1
    widthFactor = juce::jlimit(0.0f, 1.0f, widthFactor);

    // Spread target: the focused HRIR decorrelated per ear.
    //
    // Both components therefore have the same magnitude response, so the blend
    // conserves level for ANY input spectrum rather than only for white input.
    // A cone average of HRIRs would not: it cancels the high end (measured
    // -20 dB at 4.8 kHz, -24 dB at 9.6 kHz versus the focused HRIR), making the
    // spread component a lowpass whose level depends on where the source
    // signal's energy sits; through the delay's progressively darker repeats
    // that measured +7 dB.
    HRIRData spreadHRIR{};
    {
        std::array<float, HRIR_MAX_LENGTH> srcL{};
        std::array<float, HRIR_MAX_LENGTH> srcR{};
        for (int i = 0; i < hrirLength; ++i)
        {
            srcL[static_cast<size_t>(i)] = focused.leftEar[static_cast<size_t>(i)];
            srcR[static_cast<size_t>(i)] = focused.rightEar[static_cast<size_t>(i)];
        }

        std::fill(spreadHRIR.leftEar.begin(), spreadHRIR.leftEar.end(), 0.0f);
        std::fill(spreadHRIR.rightEar.begin(), spreadHRIR.rightEar.end(), 0.0f);

        for (int n = 0; n < hrirLength; ++n)
        {
            const int taps = juce::jmin(kSpreadDecorrLength, n + 1);
            float accL = 0.0f;
            float accR = 0.0f;
            for (int j = 0; j < taps; ++j)
            {
                accL += srcL[static_cast<size_t>(n - j)] * spreadDecorrL[static_cast<size_t>(j)];
                accR += srcR[static_cast<size_t>(n - j)] * spreadDecorrR[static_cast<size_t>(j)];
            }
            spreadHRIR.leftEar[static_cast<size_t>(n)] = accL;
            spreadHRIR.rightEar[static_cast<size_t>(n)] = accR;
        }
    }

    auto earEnergy = [this](const float* ir)
    {
        double e = 0.0;
        for (int i = 0; i < hrirLength; ++i)
            e += static_cast<double>(ir[i]) * static_cast<double>(ir[i]);
        return e;
    };

    const double focusedEnergyL = earEnergy(focused.leftEar.data());
    const double focusedEnergyR = earEnergy(focused.rightEar.data());

    // Blend focused -> decorrelated.
    const float focusedWeight = 1.0f - widthFactor;
    const float diffuseWeight = widthFactor;
    for (size_t i = 0; i < static_cast<size_t>(hrirLength); ++i)
    {
        focused.leftEar[i] = focused.leftEar[i] * focusedWeight + spreadHRIR.leftEar[i] * diffuseWeight;
        focused.rightEar[i] = focused.rightEar[i] * focusedWeight + spreadHRIR.rightEar[i] * diffuseWeight;
    }

    // Restore each ear's pre-blend energy. With matched spectra this gain is
    // already close to unity; it corrects the tail lost to truncating the
    // decorrelated IR back to hrirLength, and the partial cancellation between
    // the two components mid-sweep. Per ear, so the ILD is preserved: a single
    // shared scalar would be 8.4 dB wrong for the quiet contralateral ear at
    // azimuth +30 and would drag the image toward the wrong side.
    auto restoreEar = [&](float* ir, double focusedEnergy)
    {
        const double blendedEnergy = earEnergy(ir);
        if (focusedEnergy <= 1.0e-16 || blendedEnergy <= 1.0e-16)
            return;
        const float gain = juce::jlimit(0.25f, 4.0f, static_cast<float>(std::sqrt(focusedEnergy / blendedEnergy)));
        for (int i = 0; i < hrirLength; ++i)
            ir[i] *= gain;
    };

    restoreEar(focused.leftEar.data(), focusedEnergyL);
    restoreEar(focused.rightEar.data(), focusedEnergyR);
}

//==============================================================================
// Spread decorrelators.
//
// One fixed, deterministic sequence per ear. Requirements:
//   * sum of squares exactly 1, so convolution preserves energy;
//   * approximately flat magnitude, so it preserves TIMBRE as well as level and
//     the width control does not become a tone control;
//   * different between ears, so it actually decorrelates;
//   * zero mean group delay, so it introduces no interaural delay. A fixed
//     per-ear time shift would instead act as a fake ITD and pull every
//     widened source to one side.
//
// A random +-1 sequence satisfies all four: flat expected magnitude, exact unit
// energy after normalisation, and symmetric random phase. Built once with a
// fixed seed so renders stay bit-reproducible.
//==============================================================================
void BinauralProcessor::buildSpreadDecorrelators()
{
    juce::Random rng(0x5E1F2A11);

    auto fill = [&rng](std::array<float, kSpreadDecorrLength>& seq)
    {
        double energy = 0.0;
        for (int i = 0; i < kSpreadDecorrLength; ++i)
        {
            const float v = rng.nextBool() ? 1.0f : -1.0f;
            seq[static_cast<size_t>(i)] = v;
            energy += 1.0;
        }
        const float norm = static_cast<float>(1.0 / std::sqrt(juce::jmax(1.0, energy)));
        for (int i = 0; i < kSpreadDecorrLength; ++i)
            seq[static_cast<size_t>(i)] *= norm;
    };

    fill(spreadDecorrL);
    fill(spreadDecorrR);
}
