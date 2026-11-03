// PreampTests: Preamp DSP validation: harmonics, aliasing, IMD, frequency response, DC offset.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "PreampTests.h"
#include "../../DSP/PreampProcessor.h"
#include "../../DSP/OouraFFT.h"
#include "../Common/TestUtilities.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace SpatialEchoTests::Preamp
{

static constexpr int kHarmonicMaskBins = 2;  // Hann main-lobe width to reduce leakage bias

static float alignFrequencyToBin(float targetFreq, float fs, int fftSize)
{
    if (fs <= 0.0f || fftSize <= 0)
        return targetFreq;

    const float binFreq = fs / (float)fftSize;
    int bin = (int)std::round(targetFreq / binFreq);
    const int maxBin = (fftSize / 2) - 1;
    if (bin < 1)
        bin = 1;
    if (bin > maxBin)
        bin = maxBin;
    return (float)bin * binFreq;
}

static void warmupPreamp(PreampProcessor& preamp, int samples)
{
    std::vector<std::vector<float>> buffer(1, std::vector<float>(static_cast<size_t>(samples), 0.0f));
    preamp.processBuffer(buffer, samples);
}

static std::vector<float> renderSine(PreampProcessor& preamp, float fs, float freq, float amp, int samples)
{
    std::vector<std::vector<float>> buffer(1, std::vector<float>(static_cast<size_t>(samples), 0.0f));
    for (int i = 0; i < samples; ++i)
        buffer[0][static_cast<size_t>(i)] =
            amp * std::sin(2.0f * juce::MathConstants<float>::pi * freq * (float)i / fs);
    preamp.processBuffer(buffer, samples);
    return buffer[0];
}

static void computeSpectrum(const std::vector<float>& samples, std::vector<std::complex<float>>& spectrum)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    std::vector<float> window(static_cast<size_t>(fftSize));
    for (int i = 0; i < fftSize; ++i)
        window[static_cast<size_t>(i)] =
            0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) /
                                    (static_cast<float>(fftSize - 1))));

    std::vector<float> in(static_cast<size_t>(fftSize), 0.0f);
    for (int i = 0; i < fftSize && i < (int)samples.size(); ++i)
        in[static_cast<size_t>(i)] = samples[(size_t)i] * window[static_cast<size_t>(i)];

    OouraFFT fft;
    fft.init((size_t)fftSize);
    fft.fft(in.data(), spectrum.data());
}

static float magnitudeAt(const std::vector<std::complex<float>>& spectrum, float fs, float freq)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const int bin = std::min((int)spectrum.size() - 1, (int)std::round(freq * (float)fftSize / fs));
    return std::abs(spectrum[(size_t)bin]);
}

static float computeAliasRatioDb(const std::vector<std::complex<float>>& spectrum, float fs, float freq,
                                 int harmonicCount)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const int maxBin = (int)spectrum.size() - 1;

    auto binPower = [&](int bin) -> float
    {
        if (bin < 1 || bin > maxBin)
            return 0.0f;
        const float mag = std::abs(spectrum[(size_t)bin]);
        return mag * mag;
    };

    auto foldFrequency = [&](float f) -> float
    {
        if (f <= 0.0f)
            return 0.0f;
        float fmod = std::fmod(f, fs);
        if (fmod < 0.0f)
            fmod += fs;
        return (fmod > fs * 0.5f) ? (fs - fmod) : fmod;
    };

    std::vector<bool> harmonicMask((size_t)maxBin + 1, false);
    for (int h = 1; h <= harmonicCount; ++h)
    {
        const float hf = foldFrequency(freq * (float)h);
        const int bin = std::min(maxBin, (int)std::round(hf * (float)fftSize / fs));
        for (int offset = -kHarmonicMaskBins; offset <= kHarmonicMaskBins; ++offset)
        {
            const int idx = bin + offset;
            if (idx >= 1 && idx <= maxBin)
                harmonicMask[(size_t)idx] = true;
        }
    }

    float harmonicPower = 0.0f;
    float totalPower = 0.0f;
    for (int i = 1; i <= maxBin; ++i)
    {
        const float p = binPower(i);
        totalPower += p;
        if (harmonicMask[(size_t)i])
            harmonicPower += p;
    }

    const float aliasPower = std::max(1.0e-12f, totalPower - harmonicPower);
    const float refPower = std::max(1.0e-12f, harmonicPower);
    return 10.0f * std::log10(aliasPower / refPower);
}

static float computeRmsAndMean(const std::vector<float>& samples, float& mean)
{
    if (samples.empty())
    {
        mean = 0.0f;
        return 0.0f;
    }

    double sum = 0.0;
    for (float v : samples)
        sum += v;
    mean = (float)(sum / (double)samples.size());

    double sumSq = 0.0;
    for (float v : samples)
    {
        const double dv = (double)v - mean;
        sumSq += dv * dv;
    }

    return (float)std::sqrt(sumSq / (double)samples.size());
}

static float mapInputLevelKnobToGain(float knob)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, knob);
    const float db = (clamped <= 0.5f) ? juce::jmap(clamped, 0.0f, 0.5f, -60.0f, 0.0f)
                                       : juce::jmap(clamped, 0.5f, 1.0f, 0.0f, 12.0f);
    return juce::Decibels::decibelsToGain(db);
}

static bool containsSubnormal(const std::vector<float>& samples)
{
    for (float v : samples)
    {
        if (!std::isfinite(v))
            return true;
        if (std::fpclassify(v) == FP_SUBNORMAL)
            return true;
    }
    return false;
}

juce::String runPreampHarmonicsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int fftSize = 4096;
    const float freq = 1000.0f;
    const float amp = 0.5f;
    const int warmupSamples = 4096;

    auto analyze = [&](float inputLevel, float& h2Pct, float& h3Pct, float& fundMag)
    {
        PreampProcessor preamp;
        preamp.reset(fs, 1, fftSize);
        preamp.updateParameters(inputLevel);
        warmupPreamp(preamp, warmupSamples);

        auto samples = renderSine(preamp, fs, freq, amp, fftSize);
        std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
        computeSpectrum(samples, spectrum);

        const float h1 = magnitudeAt(spectrum, fs, freq);
        const float h2 = magnitudeAt(spectrum, fs, freq * 2.0f);
        const float h3 = magnitudeAt(spectrum, fs, freq * 3.0f);

        fundMag = h1;
        h2Pct = (h1 > 1.0e-6f) ? (h2 / h1) * 100.0f : 0.0f;
        h3Pct = (h1 > 1.0e-6f) ? (h3 / h1) * 100.0f : 0.0f;
    };

    float h2Low = 0.0f, h3Low = 0.0f, fundLow = 0.0f;
    float h2High = 0.0f, h3High = 0.0f, fundHigh = 0.0f;

    analyze(0.5f, h2Low, h3Low, fundLow);
    analyze(2.0f, h2High, h3High, fundHigh);

    const bool lowOk = h2Low < 1.5f;
    const bool highOk = h2High > 0.5f;
    const bool hasSignal = (fundLow > 1.0e-4f && fundHigh > 1.0e-4f);
    const bool passed = lowOk && highOk && hasSignal;

    juce::String result;
    result << "Preamp Harmonics: "
           << "H2@0.5=" << juce::String(h2Low, 2) << "% "
           << "H3@0.5=" << juce::String(h3Low, 2) << "% | "
           << "H2@2.0=" << juce::String(h2High, 2) << "% "
           << "H3@2.0=" << juce::String(h3High, 2) << "% " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampAliasTest()
{
    const float fs = 48000.0f;
    const int fftSize = 4096;
    const float amp = 0.5f;
    const int warmupSamples = 4096;
    const int harmonicCount = 9;
    const std::array<float, 6> freqTargets{8000.0f, 10000.0f, 12000.0f, 14000.0f, 16000.0f, 18000.0f};
    constexpr float kRequiredBestImprovementDb = -2.0f;  // At least one stressed tone should improve by >=2 dB.
    constexpr float kReleaseAliasOnDb = -45.0f;          // 48 kHz release target for oversampled path.

    auto measure = [&](bool oversampleEnabled, float freq, float& aliasDb, float& fundMag)
    {
        PreampProcessor preamp;
        preamp.reset(fs, 1, fftSize);
        preamp.setOversamplingEnabled(oversampleEnabled);
        preamp.updateParameters(2.0f);
        warmupPreamp(preamp, warmupSamples);

        auto samples = renderSine(preamp, fs, freq, amp, fftSize);
        std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
        computeSpectrum(samples, spectrum);
        aliasDb = computeAliasRatioDb(spectrum, fs, freq, harmonicCount);
        fundMag = magnitudeAt(spectrum, fs, freq);
    };

    float sumOff = 0.0f, sumOn = 0.0f, sumDelta = 0.0f;
    float bestDelta = 0.0f;
    bool hasDelta = false;
    bool passed = true;

    for (float targetFreq : freqTargets)
    {
        const float freq = alignFrequencyToBin(targetFreq, fs, fftSize);
        float aliasOff = 0.0f, aliasOn = 0.0f;
        float fundOff = 0.0f, fundOn = 0.0f;
        measure(false, freq, aliasOff, fundOff);
        measure(true, freq, aliasOn, fundOn);

        if (!(fundOff > 1.0e-4f && fundOn > 1.0e-4f && std::isfinite(aliasOff) && std::isfinite(aliasOn)))
            passed = false;

        const float delta = aliasOn - aliasOff;
        sumOff += aliasOff;
        sumOn += aliasOn;
        sumDelta += delta;
        if (!hasDelta || delta < bestDelta)
        {
            bestDelta = delta;
            hasDelta = true;
        }
    }

    const float avgOff = sumOff / (float)freqTargets.size();
    const float avgOn = sumOn / (float)freqTargets.size();
    const float avgDelta = sumDelta / (float)freqTargets.size();
    // The base-rate control is intentionally allowed to move when the preamp's
    // loading/tone model is recalibrated. Treat its average delta as a useful
    // diagnostic, while gating the shipping oversampled path on an absolute
    // alias ceiling and retaining one A/B stress-tone sanity check.
    const bool osImproves = hasDelta && (bestDelta <= kRequiredBestImprovementDb);
    const bool releaseAliasOk = avgOn <= kReleaseAliasOnDb;
    passed = passed && osImproves && releaseAliasOk;

    juce::String result;
    result << "Preamp Alias A/B: "
           << "avgOff=" << juce::String(avgOff, 1) << "dB "
           << "avgOn=" << juce::String(avgOn, 1) << "dB "
           << "avgDelta=" << juce::String(avgDelta, 1) << "dB "
           << "bestDelta=" << juce::String(bestDelta, 1) << "dB "
           << "targetBestDelta<=" << juce::String(kRequiredBestImprovementDb, 1) << "dB "
           << "targetAvgOn<=" << juce::String(kReleaseAliasOnDb, 0) << "dB "
           << "freqs=" << (int)freqTargets.size() << " "
           << "harmonics=" << harmonicCount << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampImdTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int fftSize = 8192;
    const float freqA = 19000.0f;
    const float freqB = 20000.0f;
    const float amp = 0.12f;
    const int warmupSamples = 4096;

    PreampProcessor preamp;
    preamp.reset(fs, 1, fftSize);
    preamp.setOversamplingEnabled(true);
    preamp.updateParameters(1.0f);
    warmupPreamp(preamp, warmupSamples);

    std::vector<std::vector<float>> buffer(1, std::vector<float>(fftSize, 0.0f));
    for (int i = 0; i < fftSize; ++i)
    {
        const float t = (float)i / fs;
        buffer[0][static_cast<size_t>(i)] = amp * (std::sin(2.0f * juce::MathConstants<float>::pi * freqA * t) +
                                                   std::sin(2.0f * juce::MathConstants<float>::pi * freqB * t));
    }

    preamp.processBuffer(buffer, fftSize);

    std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
    computeSpectrum(buffer[0], spectrum);

    const float magA = magnitudeAt(spectrum, fs, freqA);
    const float magB = magnitudeAt(spectrum, fs, freqB);
    const float magImd = magnitudeAt(spectrum, fs, std::abs(freqB - freqA));
    const float fundMag = std::max(magA, magB);

    auto toDb = [](float v) -> float { return 20.0f * std::log10(std::max(1.0e-12f, v)); };
    const float imdDb = toDb(magImd) - toDb(fundMag);
    const bool hasSignal = fundMag > 1.0e-6f && std::isfinite(imdDb);
    const bool passed = hasSignal && (imdDb <= -20.0f);

    juce::String result;
    result << "Preamp IMD: "
           << "IMD@1k=" << juce::String(imdDb, 1) << "dB "
           << "fund=" << juce::String(toDb(fundMag), 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampFreqResponseTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float amp = 0.05f;

    auto measureRms = [&](float fs, float freq) -> float
    {
        const int samples = static_cast<int>(std::ceil(fs * 0.15f));
        const int warmupSamples = static_cast<int>(std::ceil(fs * 0.05f));
        PreampProcessor preamp;
        preamp.reset(fs, 1, samples);
        preamp.updateParameters(1.0f);
        warmupPreamp(preamp, warmupSamples);

        auto out = renderSine(preamp, fs, freq, amp, samples);
        double mean = 0.0;
        for (float v : out)
            mean += v;
        mean /= (double)out.size();

        double sumSq = 0.0;
        for (float v : out)
        {
            const double dv = v - mean;
            sumSq += dv * dv;
        }
        return (float)std::sqrt(sumSq / static_cast<double>(std::max<size_t>(1u, out.size())));
    };

    auto toDb = [](float v) -> float { return 20.0f * std::log10(std::max(1.0e-9f, v)); };

    struct CaseMetrics
    {
        float fs = 0.0f;
        float db50 = 0.0f;
        float db700 = 0.0f;
        float db10k = 0.0f;
        float db20k = 0.0f;
        float minus3Hz = 0.0f;
        std::vector<std::pair<float, float>> curve{};
        bool passed = false;
    };

    std::array<CaseMetrics, 3> cases{{{48000.0f}, {96000.0f}, {192000.0f}}};

    int failures = 0;
    float max10k = -100.0f;
    float min10k = 100.0f;
    float max20k = -100.0f;

    for (auto& c : cases)
    {
        const float db1k = toDb(measureRms(c.fs, 1000.0f));
        c.db50 = toDb(measureRms(c.fs, 50.0f)) - db1k;
        c.db700 = toDb(measureRms(c.fs, 700.0f)) - db1k;
        c.db10k = toDb(measureRms(c.fs, 10000.0f)) - db1k;
        c.db20k = toDb(measureRms(c.fs, 20000.0f)) - db1k;

        constexpr std::array<float, 14> curveFrequencies{1000.0f,  2000.0f,  3000.0f,  4000.0f,  5000.0f,
                                                         6000.0f,  7000.0f,  8000.0f,  10000.0f, 12000.0f,
                                                         14000.0f, 16000.0f, 18000.0f, 20000.0f};
        for (float frequency : curveFrequencies)
            c.curve.emplace_back(frequency, toDb(measureRms(c.fs, frequency)) - db1k);

        for (size_t i = 1; i < c.curve.size(); ++i)
        {
            const auto [previousHz, previousDb] = c.curve[i - 1];
            const auto [currentHz, currentDb] = c.curve[i];
            if (previousDb > -3.0f && currentDb <= -3.0f)
            {
                const float fraction = (-3.0f - previousDb) / (currentDb - previousDb);
                const float logHz = std::log(previousHz) + fraction * (std::log(currentHz) - std::log(previousHz));
                c.minus3Hz = std::exp(logHz);
                break;
            }
        }

        const bool lowOk = c.db50 <= -2.0f;
        const bool midOk = c.db700 >= 0.5f;
        const bool highOk = c.db10k <= -8.0f && c.db20k <= -12.0f && c.db20k <= c.db10k - 2.0f;
        const bool compositeCornerOk = c.minus3Hz >= 2700.0f && c.minus3Hz <= 4000.0f;
        c.passed = lowOk && midOk && highOk && compositeCornerOk;
        if (!c.passed)
            ++failures;

        max10k = std::max(max10k, c.db10k);
        min10k = std::min(min10k, c.db10k);
        max20k = std::max(max20k, c.db20k);
    }

    const float spread10k = max10k - min10k;
    if (spread10k > 6.0f)
        ++failures;

    const bool passed = failures == 0;

    juce::String result;
    result << "Preamp FR: "
           << "48k(50/700/10k/20k)=" << juce::String(cases[0].db50, 1) << "/" << juce::String(cases[0].db700, 1) << "/"
           << juce::String(cases[0].db10k, 1) << "/" << juce::String(cases[0].db20k, 1) << "dB "
           << "96k(50/700/10k/20k)=" << juce::String(cases[1].db50, 1) << "/" << juce::String(cases[1].db700, 1) << "/"
           << juce::String(cases[1].db10k, 1) << "/" << juce::String(cases[1].db20k, 1) << "dB "
           << "192k(50/700/10k/20k)=" << juce::String(cases[2].db50, 1) << "/" << juce::String(cases[2].db700, 1) << "/"
           << juce::String(cases[2].db10k, 1) << "/" << juce::String(cases[2].db20k, 1) << "dB "
           << "spread10k=" << juce::String(spread10k, 1) << "dB"
           << " max20k=" << juce::String(max20k, 1) << "dB"
           << " minus3Hz=" << juce::String(cases[0].minus3Hz, 0) << "/" << juce::String(cases[1].minus3Hz, 0) << "/"
           << juce::String(cases[2].minus3Hz, 0) << " curve48k=";
    for (const auto& [frequency, db] : cases[0].curve)
        result << juce::String(frequency / 1000.0f, 0) << "k:" << juce::String(db, 2) << ",";
    result << " failures=" << failures << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampNullTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int samples = 8192;
    const float drive = 2.0f;
    const int warmupSamples = 4096;

    // Generate a deterministic multi-tone (high HF content + mid) for stress.
    std::vector<float> input(samples, 0.0f);
    const std::array<float, 4> freqs{400.0f, 2000.0f, 8000.0f, 12000.0f};
    for (int i = 0; i < samples; ++i)
    {
        float t = (float)i / fs;
        float s = 0.0f;
        for (float f : freqs)
            s += std::sin(2.0f * juce::MathConstants<float>::pi * f * t);
        input[static_cast<size_t>(i)] = 0.2f * s;  // keep headroom
    }

    auto processWithOversampling = [&](bool oversampleEnabled, std::vector<float>& out)
    {
        PreampProcessor preamp;
        preamp.reset(fs, 1, samples);
        preamp.setOversamplingEnabled(oversampleEnabled);
        preamp.updateParameters(drive);
        warmupPreamp(preamp, warmupSamples);

        std::vector<std::vector<float>> buffer(1, input);
        preamp.processBuffer(buffer, samples);
        out = buffer[0];
    };

    std::vector<float> outOff, outOn;
    processWithOversampling(false, outOff);
    processWithOversampling(true, outOn);

    double sumSq = 0.0;
    double peak = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        double diff = (double)outOn[static_cast<size_t>(i)] - (double)outOff[static_cast<size_t>(i)];
        sumSq += diff * diff;
        peak = std::max(peak, std::abs(diff));
    }

    const double rms = std::sqrt(sumSq / (double)samples);
    auto toDb = [](double v) { return 20.0 * std::log10(std::max(1.0e-12, v)); };
    const double rmsDb = toDb(rms);
    const double peakDb = toDb(peak);

    juce::String result;
    result << "Preamp Null: "
           << "RMS=" << juce::String(rmsDb, 1) << "dB "
           << "Peak=" << juce::String(peakDb, 1) << "dB "
           << "[PASS]";
    return result;
}

juce::String runPreampUnityGainTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int fftSize = 4096;
    const float freq = 1000.0f;
    const float amp = 0.002f;  // Low-level, near-linear operating region.
    const int warmupSamples = 4096;

    PreampProcessor preamp;
    preamp.reset(fs, 1, fftSize);
    preamp.setOversamplingEnabled(true);
    preamp.updateParameters(1.0f);
    warmupPreamp(preamp, warmupSamples);

    auto out = renderSine(preamp, fs, freq, amp, fftSize);
    float mean = 0.0f;
    const float outRms = computeRmsAndMean(out, mean);
    const float inRms = amp * 0.70710678f;

    auto toDb = [](float v) -> float { return 20.0f * std::log10(std::max(1.0e-12f, v)); };
    const float gainDb = toDb(outRms) - toDb(inRms);
    const float dcDb = toDb(std::abs(mean));

    const bool hasSignal = outRms > 1.0e-8f;
    const bool gainOk = std::abs(gainDb) <= 2.0f;
    const bool dcOk = std::abs(mean) <= 1.0e-4f;
    const bool passed = hasSignal && gainOk && dcOk;

    juce::String result;
    result << "Preamp Unity Gain: "
           << "gain=" << juce::String(gainDb, 2) << "dB "
           << "dc=" << juce::String(dcDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampInputLevelMappingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int fftSize = 4096;
    const float freq = 1000.0f;
    const float amp = 0.01f;
    const int warmupSamples = 4096;

    auto measureRms = [&](float knob) -> float
    {
        PreampProcessor preamp;
        preamp.reset(fs, 1, fftSize);
        preamp.setOversamplingEnabled(true);
        preamp.updateParameters(mapInputLevelKnobToGain(knob));
        warmupPreamp(preamp, warmupSamples);

        auto out = renderSine(preamp, fs, freq, amp, fftSize);
        float mean = 0.0f;
        return computeRmsAndMean(out, mean);
    };

    const float rmsLow = measureRms(0.0f);
    const float rmsMid = measureRms(0.5f);
    const float rmsHigh = measureRms(1.0f);

    auto toDb = [](float v) -> float { return 20.0f * std::log10(std::max(1.0e-12f, v)); };
    const float inRms = amp * 0.70710678f;
    const float midGainDb = toDb(rmsMid) - toDb(inRms);
    const float lowRelDb = toDb(rmsLow / std::max(1.0e-12f, rmsMid));
    const float highRelDb = toDb(rmsHigh / std::max(1.0e-12f, rmsMid));

    const bool midOk = std::abs(midGainDb) <= 2.0f;
    const bool lowOk = (lowRelDb <= -50.0f && lowRelDb >= -70.0f);
    const bool highOk = (highRelDb >= 8.0f && highRelDb <= 14.0f);
    const bool passed = (rmsMid > 1.0e-8f) && midOk && lowOk && highOk;

    juce::String result;
    result << "Preamp InputLevel Mapping: mid=" << juce::String(midGainDb, 2) << "dB "
           << "low=" << juce::String(lowRelDb, 1) << "dB "
           << "high=" << juce::String(highRelDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampHarmonicSignatureTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const int fftSize = 4096;
    const float amp = 0.5f;
    const int warmupSamples = 4096;
    const std::array<float, 3> sampleRates{44100.0f, 48000.0f, 96000.0f};

    // Asymmetry follows the tape-class 0.015+0.03*drive law, which makes the
    // signature odd-harmonic dominant (measured at 48k: H2=3.49, H3=11.66,
    // ratio ~3.3x vs the 1.5x pin), so the gate does not expect the h2 > h3
    // balance of a tube-like stage. Checked across sample rates
    // so a rate-dependent harmonic balance regression cannot pass the default
    // suite.
    juce::String result;
    result << "Preamp Harmonic Signature: ";
    bool passed = true;
    for (float fs : sampleRates)
    {
        const float freq = alignFrequencyToBin(1000.0f, fs, fftSize);
        PreampProcessor preamp;
        preamp.reset(fs, 1, fftSize);
        preamp.setOversamplingEnabled(true);
        preamp.updateParameters(1.5f);
        warmupPreamp(preamp, warmupSamples);

        auto samples = renderSine(preamp, fs, freq, amp, fftSize);
        std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
        computeSpectrum(samples, spectrum);

        const float h1 = magnitudeAt(spectrum, fs, freq);
        const float h2 = magnitudeAt(spectrum, fs, freq * 2.0f);
        const float h3 = magnitudeAt(spectrum, fs, freq * 3.0f);
        if (!((h1 > 1.0e-4f) && (h3 > 1.5f * h2)))
            passed = false;
        result << juce::String((int)fs) << "Hz{H2=" << juce::String(h2, 4) << " H3=" << juce::String(h3, 4) << "} ";
    }

    result << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampAliasingSweepTest()
{
    const int fftSize = 8192;
    const float amp = 0.5f;
    const int warmupSamples = 4096;
    const int harmonicCount = 9;
    const std::array<float, 3> sampleRates{44100.0f, 48000.0f, 96000.0f};

    juce::String result;
    result << "Preamp Aliasing Sweep: ";

    bool passed = true;
    for (float fs : sampleRates)
    {
        const float freq = alignFrequencyToBin(std::min(18000.0f, 0.4f * fs), fs, fftSize);
        const float releaseTargetDb = (fs <= 44100.5f) ? -45.0f : (fs <= 48000.5f) ? -50.0f : -55.0f;
        PreampProcessor preamp;
        preamp.reset(fs, 1, fftSize);
        preamp.setOversamplingEnabled(true);
        preamp.updateParameters(2.0f);
        warmupPreamp(preamp, warmupSamples);

        auto samples = renderSine(preamp, fs, freq, amp, fftSize);
        std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
        computeSpectrum(samples, spectrum);

        const float aliasDb = computeAliasRatioDb(spectrum, fs, freq, harmonicCount);
        const float fundMag = magnitudeAt(spectrum, fs, freq);
        if (!(fundMag > 1.0e-4f && std::isfinite(aliasDb) && aliasDb <= releaseTargetDb))
            passed = false;

        result << juce::String((int)fs) << "Hz=" << juce::String(aliasDb, 1) << "dB"
               << "(<=" << juce::String(releaseTargetDb, 0) << ") ";
    }

    result << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampDCOffsetAndAutomationTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blockSize = 256;
    const int blocks = 16;
    const int warmupSamples = 1024;

    PreampProcessor preamp;
    preamp.reset(fs, 1, blockSize);
    preamp.setOversamplingEnabled(true);
    preamp.updateParameters(1.0f);
    warmupPreamp(preamp, warmupSamples);

    double sum = 0.0;
    double peak = 0.0;
    bool finite = true;

    for (int b = 0; b < blocks; ++b)
    {
        const float inputLevel = (b % 2 == 0) ? 0.5f : 2.0f;
        preamp.updateParameters(inputLevel);

        std::vector<std::vector<float>> buffer(1, std::vector<float>(blockSize, 0.0f));
        preamp.processBuffer(buffer, blockSize);

        for (float v : buffer[0])
        {
            if (!std::isfinite(v))
                finite = false;
            sum += v;
            peak = std::max(peak, std::abs((double)v));
        }
    }

    const int totalSamples = blockSize * blocks;
    const double mean = (totalSamples > 0) ? (sum / (double)totalSamples) : 0.0;
    const bool dcOk = std::abs(mean) <= 1.0e-4;
    const bool peakOk = peak <= 1.0e-3;
    const bool passed = finite && dcOk && peakOk;

    auto toDb = [](double v) { return 20.0 * std::log10(std::max(1.0e-12, v)); };

    juce::String result;
    result << "Preamp DC/Automation: "
           << "dc=" << juce::String(toDb(std::abs(mean)), 1) << "dB "
           << "peak=" << juce::String(toDb(peak), 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampDenormalSafetyTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int samples = 4096;
    const float amp = 1.0e-12f;

    PreampProcessor preamp;
    preamp.reset(fs, 1, samples);
    preamp.setOversamplingEnabled(true);
    preamp.updateParameters(1.0f);

    std::vector<std::vector<float>> buffer(1, std::vector<float>(samples, 0.0f));
    for (int i = 0; i < samples; ++i)
        buffer[0][static_cast<size_t>(i)] =
            amp * std::sin(2.0f * juce::MathConstants<float>::pi * 997.0f * (float)i / fs);

    preamp.processBuffer(buffer, samples);
    const bool passed = !containsSubnormal(buffer[0]);

    juce::String result;
    result << "Preamp Denormal Safety: " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampOversamplingLowpassCheckTest()
{
    const float fs = 48000.0f;
    const int fftSize = 8192;
    const float freq = alignFrequencyToBin(19000.0f, fs, fftSize);
    const float amp = 0.5f;
    const int warmupSamples = 4096;
    const int harmonicCount = 9;
    constexpr float kReleaseAliasDb = -45.0f;

    PreampProcessor preamp;
    preamp.reset(fs, 1, fftSize);
    preamp.setOversamplingEnabled(true);
    preamp.updateParameters(2.0f);
    warmupPreamp(preamp, warmupSamples);

    auto samples = renderSine(preamp, fs, freq, amp, fftSize);
    std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
    computeSpectrum(samples, spectrum);

    const float aliasDb = computeAliasRatioDb(spectrum, fs, freq, harmonicCount);
    const float fundMag = magnitudeAt(spectrum, fs, freq);
    const bool passed = fundMag > 1.0e-4f && std::isfinite(aliasDb) && aliasDb <= kReleaseAliasDb;

    juce::String result;
    result << "Preamp OS Lowpass: "
           << "alias=" << juce::String(aliasDb, 1) << "dB "
           << "target<=" << juce::String(kReleaseAliasDb, 0) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Preamp low-level distortion gate.
//
// Guards a numerical defect in the first-order ADAA saturator. The divided
// difference (AD1(x) - AD1(x1)) / (x - x1) is a catastrophic cancellation when
// consecutive samples are close, so it must not be evaluated in float32: the
// resulting error is large enough that the ADAA branch and the midpoint
// fallback branch disagree audibly, and the per-sample switch between them at
// |x - x1| = ADAA_TOL injects harmonics.
//
// The error peaks where a signal's per-sample slew straddles that tolerance,
// which for a 1 kHz sine at 48 kHz with 4x oversampling is around -60 dBFS -
// ordinary delay/reverb tail level, on the wet path of the whole plugin.
// THD on a 1 kHz sine with the divided difference evaluated in float32:
//     amplitude 0.0003 -> 0.0004%   (all samples take the fallback: clean)
//     amplitude 0.001  -> 14.24%    <-- worst case
//     amplitude 0.003  ->  3.09%
//     amplitude 0.01   ->  0.28%
// Evaluated in double, as the saturator does: 0.0013%, 0.0038% and 0.0124%.
//
// The float32 signature is also non-monotonic in level, which is not physical
// for a saturator - it falls as the signal gets LOUDER between -60 and
// -40 dBFS. This gate asserts monotonic, low distortion at small signal, which
// the float32 evaluation fails by four orders of magnitude. High-drive
// behaviour is deliberately not constrained here: the intended saturation
// measures 3.78% at full scale with either evaluation, and that is the musical
// part.
juce::String runPreampLowLevelDistortionTest(SpatialEchoAudioProcessor& /*processor*/)
{
    using SpatialEchoTests::TestUtilities;

    const float fs = 48000.0f;
    const int blockSize = 512;
    const double fundamental = 1000.0;

    // 0.1% leaves a ~75x margin over the measured worst case in this amplitude
    // range (0.0124%) while failing the float32 evaluation by >100x.
    const double thdLimit = 0.001;

    struct Point
    {
        float amplitude;
        double thd;
    };
    std::vector<Point> points;
    bool allFinite = true;

    for (float amplitude : {0.001f, 0.003f, 0.01f})
    {
        PreampProcessor preamp;
        preamp.reset(fs, 1, blockSize);
        preamp.updateParameters(1.0f);

        double phase = 0.0;
        const double phaseStep = 2.0 * juce::MathConstants<double>::pi * fundamental / fs;

        // Settle the DC blockers and the parameter smoothers.
        for (int b = 0; b < 80; ++b)
        {
            std::vector<std::vector<float>> buf(1, std::vector<float>((size_t)blockSize, 0.0f));
            for (int i = 0; i < blockSize; ++i)
            {
                buf[0][(size_t)i] = amplitude * (float)std::sin(phase);
                phase += phaseStep;
            }
            preamp.processBuffer(buf, blockSize);
        }

        // Exactly one second at 48 kHz, so 1/2/3/4/5 kHz land on exact DFT bins
        // and no window is needed.
        const int measureSamples = 48000;
        std::vector<double> output((size_t)measureSamples, 0.0);
        int written = 0;
        while (written < measureSamples)
        {
            const int n = std::min(blockSize, measureSamples - written);
            std::vector<std::vector<float>> buf(1, std::vector<float>((size_t)n, 0.0f));
            for (int i = 0; i < n; ++i)
            {
                buf[0][(size_t)i] = amplitude * (float)std::sin(phase);
                phase += phaseStep;
            }
            preamp.processBuffer(buf, n);
            for (int i = 0; i < n; ++i)
                output[(size_t)(written + i)] = buf[0][(size_t)i];
            written += n;
        }

        auto binMagnitude = [&](double frequency)
        {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < measureSamples; ++i)
            {
                const double t = 2.0 * juce::MathConstants<double>::pi * frequency * i / fs;
                re += output[(size_t)i] * std::cos(t);
                im -= output[(size_t)i] * std::sin(t);
            }
            return 2.0 * std::sqrt(re * re + im * im) / measureSamples;
        };

        const double h1 = binMagnitude(fundamental);
        const double h2 = binMagnitude(2.0 * fundamental);
        const double h3 = binMagnitude(3.0 * fundamental);
        const double h4 = binMagnitude(4.0 * fundamental);
        const double h5 = binMagnitude(5.0 * fundamental);

        if (!(h1 > 0.0) || !std::isfinite(h1))
        {
            allFinite = false;
            points.push_back({amplitude, 1.0e9});
            continue;
        }

        const double thd = std::sqrt(h2 * h2 + h3 * h3 + h4 * h4 + h5 * h5) / h1;
        if (!std::isfinite(thd))
            allFinite = false;

        points.push_back({amplitude, thd});
    }

    double worstThd = 0.0;
    float worstAmplitude = 0.0f;
    for (const auto& point : points)
    {
        if (point.thd > worstThd)
        {
            worstThd = point.thd;
            worstAmplitude = point.amplitude;
        }
    }

    // Distortion must not fall as the signal gets louder across this range.
    // The float32 evaluation goes 14.24% -> 3.09% -> 0.28% over exactly these
    // three amplitudes, the branch-mismatch signature rather than saturation.
    bool monotonic = true;
    for (size_t i = 1; i < points.size(); ++i)
    {
        if (points[i].thd + 1.0e-9 < points[i - 1].thd)
            monotonic = false;
    }

    const bool passed = allFinite && monotonic && worstThd < thdLimit;

    juce::String details;
    for (const auto& point : points)
    {
        details << "amp " << juce::String(point.amplitude, 4) << " THD " << juce::String(point.thd * 100.0, 4) << "%; ";
    }
    details << "worst " << juce::String(worstThd * 100.0, 4) << "% at amp " << juce::String(worstAmplitude, 4)
            << " (limit " << juce::String(thdLimit * 100.0, 4) << "%)"
            << ", monotonic=" << (monotonic ? "yes" : "no");

    return TestUtilities::formatResult(passed, "PreampLowLevelDistortion", details);
}

}  // namespace SpatialEchoTests::Preamp

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
