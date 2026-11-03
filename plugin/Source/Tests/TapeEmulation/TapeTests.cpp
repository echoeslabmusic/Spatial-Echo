// TapeTests: Unit tests for tape emulation (record saturation, modulation, tone, and distortion).

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "TapeTests.h"
#include "../../PluginProcessor.h"
#include "../../DSP/EchoesModel.h"
#include "../../DSP/PreampProcessor.h"
#include "../../DSP/TapeDelay.h"
#include "../../DSP/TapeFlavors.h"
#include "../../DSP/OouraFFT.h"
#include "../../DSP/dependencies/RbjBiquad.h"
#include "../System/EchoesModelTestAccess.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace SpatialEchoTests::TapeEmulation
{

// Release/HQ aliasing + IMD targets used by this test harness.
// Keep thresholds synchronized with current product goals and measured baselines.
static constexpr float kAliasRatioReleaseDb = -45.0f;  // alias-to-harmonic
static constexpr float kAliasRatioHqDb = -55.0f;
static constexpr float kAliasAbsReleaseDb = -70.0f;  // alias-to-total
static constexpr float kAliasAbsHqDb = -80.0f;
static constexpr float kImdReleaseDb = -40.0f;  // IMD difference-tone vs fundamental
static constexpr float kImdHqDb = -50.0f;
static constexpr int kHarmonicMaskBins = 2;  // Hann main-lobe width to reduce leakage bias

static float alignFrequencyToBin(float targetFreq, float fs, int fftSize)
{
    if (fs <= 0.0f || fftSize <= 0)
        return targetFreq;
    const float binFreq = fs / static_cast<float>(fftSize);
    int bin = static_cast<int>(std::round(targetFreq / binFreq));
    const int maxBin = (fftSize / 2) - 1;
    if (bin < 1)
        bin = 1;
    if (bin > maxBin)
        bin = maxBin;
    return static_cast<float>(bin) * binFreq;
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

static float computeTwoToneImdDb(const std::vector<float>& samples, float fs, float freqA, float freqB,
                                 float minFundMag)
{
    if (samples.size() < 4 || fs <= 0.0f)
        return std::numeric_limits<float>::quiet_NaN();

    std::vector<std::complex<float>> spectrum(samples.size() / 2 + 1);
    computeSpectrum(samples, spectrum);

    const float magA = magnitudeAt(spectrum, fs, freqA);
    const float magB = magnitudeAt(spectrum, fs, freqB);
    const float fundMag = std::max(magA, magB);
    const float magImd = magnitudeAt(spectrum, fs, std::abs(freqB - freqA));

    if (fundMag <= minFundMag)
        return std::numeric_limits<float>::quiet_NaN();

    return 20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
}

// A stationary single-tone residual estimate. Only actual in-band harmonics
// belong to the wanted signal; folding an out-of-band harmonic into this mask
// would incorrectly exempt the alias we are trying to measure. Modulation,
// noise and leakage also contribute to the residual. Aliases coincident with
// wanted bins require a separate high-rate reference and cannot be isolated here.
struct AliasPowers
{
    double harmonic = 0.0;
    double residual = 0.0;
    bool valid = false;
};

static AliasPowers measureAliasPowers(const std::vector<std::complex<float>>& spectrum, float fs, float freq,
                                      int harmonicCount)
{
    AliasPowers powers;
    if (spectrum.size() < 3 || !std::isfinite(fs) || !std::isfinite(freq) || fs <= 0.0f || freq <= 0.0f ||
        freq >= fs * 0.5f || harmonicCount < 1)
        return powers;

    for (const auto& value : spectrum)
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            return powers;

    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const int maxBin = static_cast<int>(spectrum.size()) - 1;
    std::vector<bool> harmonicMask(static_cast<size_t>(maxBin + 1), false);
    for (int harmonic = 1; harmonic <= harmonicCount; ++harmonic)
    {
        const double frequency = static_cast<double>(freq) * harmonic;
        if (frequency > static_cast<double>(fs) * 0.5)
            break;

        const int bin = static_cast<int>(std::round(frequency * fftSize / fs));
        for (int offset = -kHarmonicMaskBins; offset <= kHarmonicMaskBins; ++offset)
        {
            const int index = bin + offset;
            if (index >= 1 && index <= maxBin)
                harmonicMask[static_cast<size_t>(index)] = true;
        }
    }

    // DC is deliberately omitted after time-domain DC removal. Accumulate
    // disjoint powers directly in double precision: total - harmonic in float
    // erased small but measurable aliases through cancellation.
    for (int bin = 1; bin <= maxBin; ++bin)
    {
        const auto value = spectrum[static_cast<size_t>(bin)];
        const double power =
            static_cast<double>(value.real()) * value.real() + static_cast<double>(value.imag()) * value.imag();
        if (harmonicMask[static_cast<size_t>(bin)])
            powers.harmonic += power;
        else
            powers.residual += power;
    }
    powers.valid = powers.harmonic > 0.0 && std::isfinite(powers.harmonic) && std::isfinite(powers.residual);
    return powers;
}

static float computeAliasRatioDb(const std::vector<std::complex<float>>& spectrum, float fs, float freq,
                                 int harmonicCount)
{
    const auto powers = measureAliasPowers(spectrum, fs, freq, harmonicCount);
    if (!powers.valid)
        return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(10.0 * std::log10(std::max(1.0e-24, powers.residual / powers.harmonic)));
}

static float computeAliasToTotalDb(const std::vector<std::complex<float>>& spectrum, float fs, float freq,
                                   int harmonicCount)
{
    const auto powers = measureAliasPowers(spectrum, fs, freq, harmonicCount);
    if (!powers.valid)
        return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(10.0 *
                              std::log10(std::max(1.0e-24, powers.residual / (powers.harmonic + powers.residual))));
}

// Analytic positive/negative controls qualify the metric before a production
// render can pass it. Bin 3563 is the folded third harmonic of bin 1543 at
// N=8192; it must be residual, while the second harmonic at 3086 is wanted.
static bool qualifyAliasEstimator(double& worstErrorDb)
{
    constexpr int fftSize = 8192;
    constexpr float fs = 48000.0f;
    constexpr int fundamentalBin = 1543;
    constexpr int aliasBin = fftSize - 3 * fundamentalBin;
    constexpr float frequency = fundamentalBin * fs / fftSize;
    worstErrorDb = 0.0;
    bool passed = true;
    for (float scale : {1.0e-10f, 1.0f, 5.0f})
    {
        std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
        spectrum[fundamentalBin] = scale;
        spectrum[2 * fundamentalBin] = 0.2f * scale;
        passed = passed && juce::exactlyEqual(computeAliasRatioDb(spectrum, fs, frequency, 6), -240.0f);
        passed = passed && juce::exactlyEqual(computeAliasToTotalDb(spectrum, fs, frequency, 6), -240.0f);
        for (float aliasAmplitude : {0.1f, 0.001f})
        {
            spectrum[aliasBin] = aliasAmplitude * scale;
            const double aliasPower = static_cast<double>(aliasAmplitude) * aliasAmplitude;
            const double expectedRatio = 10.0 * std::log10(aliasPower / 1.04);
            const double expectedTotal = 10.0 * std::log10(aliasPower / (1.04 + aliasPower));
            const double ratio = computeAliasRatioDb(spectrum, fs, frequency, 6);
            const double total = computeAliasToTotalDb(spectrum, fs, frequency, 6);
            const double error = std::max(std::abs(ratio - expectedRatio), std::abs(total - expectedTotal));
            passed = passed && std::isfinite(ratio) && std::isfinite(total) && error <= 0.001;
            worstErrorDb = std::max(worstErrorDb, error);
        }
    }

    std::vector<std::complex<float>> invalid(fftSize / 2 + 1);
    passed = passed && !std::isfinite(computeAliasRatioDb(invalid, fs, frequency, 6));
    passed = passed && !std::isfinite(computeAliasToTotalDb(invalid, fs, frequency, 6));
    invalid[fundamentalBin] = 1.0f;
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        invalid[0] = value;
        passed = passed && !std::isfinite(computeAliasRatioDb(invalid, fs, frequency, 6));
        passed = passed && !std::isfinite(computeAliasToTotalDb(invalid, fs, frequency, 6));
    }
    return passed;
}

juce::String runTapeAliasEstimatorQualificationTest(SpatialEchoAudioProcessor& /*processor*/)
{
    double analyticErrorDb = 0.0;
    const bool analyticPassed = qualifyAliasEstimator(analyticErrorDb);
    constexpr int count = 8192;
    constexpr float fs = 48000.0f;
    constexpr int bin = 1543;
    constexpr float frequency = bin * fs / count;
    std::vector<float> samples(count);
    for (int i = 0; i < count; ++i)
    {
        const double phase = 2.0 * juce::MathConstants<double>::pi * bin * i / count;
        samples[static_cast<size_t>(i)] = static_cast<float>(std::sin(phase) + 0.2 * std::sin(2.0 * phase));
    }
    std::vector<std::complex<float>> spectrum(count / 2 + 1);
    computeSpectrum(samples, spectrum);
    const float cleanDb = computeAliasRatioDb(spectrum, fs, frequency, 6);
    for (int i = 0; i < count; ++i)
    {
        const double phase = 2.0 * juce::MathConstants<double>::pi * (count - 3 * bin) * i / count;
        samples[static_cast<size_t>(i)] += static_cast<float>(0.1 * std::sin(phase));
    }
    computeSpectrum(samples, spectrum);
    const float injectedDb = computeAliasRatioDb(spectrum, fs, frequency, 6);
    const double expectedDb = 10.0 * std::log10(0.01 / 1.04);
    const bool passed = analyticPassed && std::isfinite(cleanDb) && cleanDb < -80.0f && std::isfinite(injectedDb) &&
                        std::abs(injectedDb - expectedDb) < 0.001;
    return "TapeAliasEstimatorQualification: analyticError=" + juce::String(analyticErrorDb, 6) +
           "dB windowClean=" + juce::String(cleanDb, 2) + "dB injected=" + juce::String(injectedDb, 4) + "dB " +
           (passed ? "[PASS]" : "[FAIL]");
}

static void configureStressModel(EchoesModel& model, int heads[3], float intensity, float tapeDrive,
                                 float tapeSaturation, float tapeBias)
{
    model.UpdateParameters(0.5f, 0.5f, intensity, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f,
                           0.0f,                     // noise disabled, pristine tape
                           0.0f, 0.0f, 0.0f,         // wow/flutter disabled
                           0,                        // stereo mode
                           false, 0.0f, 0.0f, 0.0f,  // tempo sync off
                           false, 0.0f, 0.0f,        // binaural off
                           1.0f, 100.0f, 150.0f, 2,  // binaural defaults
                           tapeDrive, tapeSaturation, tapeBias, 0, 0);
    if (auto* tapeDelay = model.getTapeDelay())
    {
        tapeDelay->setTapeCharacter(tapeDrive, tapeSaturation, tapeBias);
        tapeDelay->setTapeRecordMakeupGainDb(0.0f);
    }
}

static void processToneBlocks(EchoesModel& model, std::vector<std::vector<float>>& buf, int blockSize, int blocks,
                              float fs, float freqA, float ampA, float freqB, float ampB, float& phaseA, float& phaseB,
                              std::vector<float>* output)
{
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float incA = twoPi * freqA / fs;
    const float incB = twoPi * freqB / fs;

    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            float sample = ampA * std::sin(phaseA);
            if (freqB > 0.0f && ampB > 0.0f)
                sample += ampB * std::sin(phaseB);

            phaseA += incA;
            if (phaseA >= twoPi)
                phaseA -= twoPi;
            if (freqB > 0.0f && ampB > 0.0f)
            {
                phaseB += incB;
                if (phaseB >= twoPi)
                    phaseB -= twoPi;
            }

            buf[0][static_cast<size_t>(s)] = sample;
            buf[1][static_cast<size_t>(s)] = sample;
        }

        model.ProcessBuffer(buf, blockSize);
        if (output)
            output->insert(output->end(), buf[0].begin(), buf[0].end());
    }
}

static void removeDc(std::vector<float>& samples);

static float measureFullNoFeedbackImd(float fs, int fftSize, int blockSize, int warmBlocks, float freqA, float freqB,
                                      float amp, float minFundMag)
{
    const int captureBlocks = fftSize / blockSize;
    const float noFeedbackIntensity = 0.0f;
    const float tapeDrive = 1.0f;
    const float tapeSaturation = 1.0f;
    const float tapeBias = 0.5f;
    int heads[3] = {1, 0, 0};

    EchoesModel model;
    model.Reset(fs, 2, blockSize);
    configureStressModel(model, heads, noFeedbackIntensity, tapeDrive, tapeSaturation, tapeBias);

    std::vector<std::vector<float>> buf(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    float phaseA = 0.0f;
    float phaseB = 0.0f;
    processToneBlocks(model, buf, blockSize, warmBlocks, fs, freqA, amp, freqB, amp, phaseA, phaseB, nullptr);

    std::vector<float> samples;
    samples.reserve((size_t)fftSize);
    processToneBlocks(model, buf, blockSize, captureBlocks, fs, freqA, amp, freqB, amp, phaseA, phaseB, &samples);
    samples.resize((size_t)fftSize);
    removeDc(samples);

    return computeTwoToneImdDb(samples, fs, freqA, freqB, minFundMag);
}

static void processToneBlocksPreamp(PreampProcessor& preamp, std::vector<std::vector<float>>& buf, int blockSize,
                                    int blocks, float fs, float freqA, float ampA, float freqB, float ampB,
                                    float& phaseA, float& phaseB, std::vector<float>* output)
{
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float incA = twoPi * freqA / fs;
    const float incB = twoPi * freqB / fs;

    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            float sample = ampA * std::sin(phaseA);
            if (freqB > 0.0f && ampB > 0.0f)
                sample += ampB * std::sin(phaseB);

            phaseA += incA;
            if (phaseA >= twoPi)
                phaseA -= twoPi;
            if (freqB > 0.0f && ampB > 0.0f)
            {
                phaseB += incB;
                if (phaseB >= twoPi)
                    phaseB -= twoPi;
            }

            buf[0][static_cast<size_t>(s)] = sample;
            buf[1][static_cast<size_t>(s)] = sample;
        }

        preamp.processBuffer(buf, blockSize);
        if (output)
            output->insert(output->end(), buf[0].begin(), buf[0].end());
    }
}

static void processToneBlocksDelayOnly(TapeDelayProcessor& delay, std::vector<std::vector<float>>& buf, int blockSize,
                                       int blocks, float fs, float freqA, float ampA, float freqB, float ampB,
                                       float& phaseA, float& phaseB, std::vector<float>* output)
{
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float incA = twoPi * freqA / fs;
    const float incB = twoPi * freqB / fs;

    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            float sample = ampA * std::sin(phaseA);
            if (freqB > 0.0f && ampB > 0.0f)
                sample += ampB * std::sin(phaseB);

            phaseA += incA;
            if (phaseA >= twoPi)
                phaseA -= twoPi;
            if (freqB > 0.0f && ampB > 0.0f)
            {
                phaseB += incB;
                if (phaseB >= twoPi)
                    phaseB -= twoPi;
            }

            buf[0][static_cast<size_t>(s)] = sample;
            buf[1][static_cast<size_t>(s)] = sample;
        }

        delay.ProcessBuffer(buf, blockSize);
        if (output)
            output->insert(output->end(), buf[0].begin(), buf[0].end());
    }
}

static float measurePreampOnlyImd(float fs, int fftSize, int blockSize, int warmBlocks, float freqA, float freqB,
                                  float amp, float minFundMag)
{
    const int captureBlocks = fftSize / blockSize;
    PreampProcessor preamp;
    preamp.reset(fs, 2, blockSize);
    preamp.updateParameters(1.0f);

    std::vector<std::vector<float>> buf(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    float phaseA = 0.0f;
    float phaseB = 0.0f;
    processToneBlocksPreamp(preamp, buf, blockSize, warmBlocks, fs, freqA, amp, freqB, amp, phaseA, phaseB, nullptr);

    std::vector<float> samples;
    samples.reserve((size_t)fftSize);
    processToneBlocksPreamp(preamp, buf, blockSize, captureBlocks, fs, freqA, amp, freqB, amp, phaseA, phaseB,
                            &samples);
    samples.resize((size_t)fftSize);
    removeDc(samples);

    return computeTwoToneImdDb(samples, fs, freqA, freqB, minFundMag);
}

static float measureDelayOnlyNoFeedbackImd(float fs, int fftSize, int blockSize, int warmBlocks, float freqA,
                                           float freqB, float amp, float minFundMag)
{
    const int captureBlocks = fftSize / blockSize;
    const float tapeDrive = 1.0f;
    const float tapeSaturation = 1.0f;
    const float tapeBias = 0.5f;
    TapeDelayProcessor delay;
    delay.Reset(fs, 2);
    delay.setTapeCharacter(tapeDrive, tapeSaturation, tapeBias);
    delay.setTapeRecordMakeupGainDb(0.0f);

    int headsOnly[3] = {1, 0, 0};
    delay.UpdateParameters(0.0f, 0.5f, headsOnly, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f,
                           0.0f);

    std::vector<std::vector<float>> buf(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    float phaseA = 0.0f;
    float phaseB = 0.0f;
    processToneBlocksDelayOnly(delay, buf, blockSize, warmBlocks, fs, freqA, amp, freqB, amp, phaseA, phaseB, nullptr);

    std::vector<float> samples;
    samples.reserve((size_t)fftSize);
    processToneBlocksDelayOnly(delay, buf, blockSize, captureBlocks, fs, freqA, amp, freqB, amp, phaseA, phaseB,
                               &samples);
    samples.resize((size_t)fftSize);
    removeDc(samples);

    return computeTwoToneImdDb(samples, fs, freqA, freqB, minFundMag);
}

static void processRecordChainBlocks(EchoesModel& model, std::vector<std::vector<float>>& buf, int blockSize,
                                     int blocks, float fs, float freq, float amp, float& phase,
                                     std::vector<float>* output)
{
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float inc = twoPi * freq / fs;

    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            float sample = amp * std::sin(phase);
            phase += inc;
            if (phase >= twoPi)
                phase -= twoPi;
            buf[0][static_cast<size_t>(s)] = sample;
            buf[1][static_cast<size_t>(s)] = sample;
        }

        EchoesModelTestAccess::processRecordChainOnly(model, buf, blockSize);
        if (output)
            output->insert(output->end(), buf[0].begin(), buf[0].end());
    }
}

static void processRecordChainToneBlocks(EchoesModel& model, std::vector<std::vector<float>>& buf, int blockSize,
                                         int blocks, float fs, float freqA, float ampA, float freqB, float ampB,
                                         float& phaseA, float& phaseB, std::vector<float>* output)
{
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float incA = twoPi * freqA / fs;
    const float incB = twoPi * freqB / fs;

    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            float sample = ampA * std::sin(phaseA);
            if (freqB > 0.0f && ampB > 0.0f)
                sample += ampB * std::sin(phaseB);

            phaseA += incA;
            if (phaseA >= twoPi)
                phaseA -= twoPi;
            if (freqB > 0.0f && ampB > 0.0f)
            {
                phaseB += incB;
                if (phaseB >= twoPi)
                    phaseB -= twoPi;
            }

            buf[0][static_cast<size_t>(s)] = sample;
            buf[1][static_cast<size_t>(s)] = sample;
        }

        EchoesModelTestAccess::processRecordChainOnly(model, buf, blockSize);
        if (output)
            output->insert(output->end(), buf[0].begin(), buf[0].end());
    }
}

static void processPlaybackOnlyToneBlocks(EchoesModel& model, std::vector<std::vector<float>>& buf, int blockSize,
                                          int blocks, float fs, float freqA, float ampA, float freqB, float ampB,
                                          float& phaseA, float& phaseB, std::vector<float>* output)
{
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float incA = twoPi * freqA / fs;
    const float incB = twoPi * freqB / fs;

    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            float sample = ampA * std::sin(phaseA);
            if (freqB > 0.0f && ampB > 0.0f)
                sample += ampB * std::sin(phaseB);

            phaseA += incA;
            if (phaseA >= twoPi)
                phaseA -= twoPi;
            if (freqB > 0.0f && ampB > 0.0f)
            {
                phaseB += incB;
                if (phaseB >= twoPi)
                    phaseB -= twoPi;
            }

            buf[0][static_cast<size_t>(s)] = sample;
            buf[1][static_cast<size_t>(s)] = sample;
        }

        EchoesModelTestAccess::processPlaybackOnly(model, buf, blockSize);
        if (output)
            output->insert(output->end(), buf[0].begin(), buf[0].end());
    }
}

static void removeDc(std::vector<float>& samples)
{
    if (samples.empty())
        return;
    double sum = 0.0;
    for (float v : samples)
        sum += v;
    const float mean = static_cast<float>(sum / static_cast<double>(samples.size()));
    for (float& v : samples)
        v -= mean;
}

static void bandLimitTone(std::vector<float>& samples, float fs)
{
    if (samples.empty() || fs <= 0.0f)
        return;

    RbjBiquad hpf;
    RbjBiquad lpf;
    hpf.Init(BiquadType::HPF, 1000.0f, fs, 0.707f, 0.0f);
    lpf.Init(BiquadType::LPF, 6000.0f, fs, 0.707f, 0.0f);
    hpf.Reset(fs);
    lpf.Reset(fs);

    for (float& v : samples)
        v = lpf.ProcessSample(hpf.ProcessSample(v));
}

struct FrequencyStats
{
    float meanHz = 0.0f;
    float stdDevHz = 0.0f;
    int periods = 0;
};

static FrequencyStats measureZeroCrossingFrequency(const std::vector<float>& samples, float fs, float expectedHz)
{
    FrequencyStats stats{};
    if (samples.size() < 2 || fs <= 0.0f || expectedHz <= 0.0f)
        return stats;

    const float minFreq = expectedHz * 0.7f;
    const float maxFreq = expectedHz * 1.3f;

    float prev = samples[0];
    float lastCross = -1.0f;
    double mean = 0.0;
    double m2 = 0.0;
    int count = 0;

    for (size_t i = 1; i < samples.size(); ++i)
    {
        const float curr = samples[i];
        if (prev <= 0.0f && curr > 0.0f)
        {
            const float denom = prev - curr;
            float frac = 0.0f;
            if (std::abs(denom) > 1.0e-12f)
                frac = prev / denom;

            const float crossing = static_cast<float>(i - 1) + frac;
            if (lastCross >= 0.0f)
            {
                const float period = crossing - lastCross;
                if (period > 0.0f)
                {
                    const float freq = fs / period;
                    if (freq >= minFreq && freq <= maxFreq)
                    {
                        ++count;
                        const double delta = freq - mean;
                        mean += delta / count;
                        const double delta2 = freq - mean;
                        m2 += delta * delta2;
                    }
                }
            }
            lastCross = crossing;
        }
        prev = curr;
    }

    stats.periods = count;
    if (count > 0)
    {
        stats.meanHz = static_cast<float>(mean);
        stats.stdDevHz = (count > 1) ? static_cast<float>(std::sqrt(m2 / (count - 1))) : 0.0f;
    }

    return stats;
}

//==============================================================================
// Pitch Modulation Tests
//==============================================================================

namespace
{
inline float mapRateParamToScale(float param)
{
    const float clamped = juce::jlimit(0.0f, 1.0f, param);
    return std::pow(2.0f, (clamped - 0.5f) * 4.0f);
}

inline float mapRateScaleToParam(float scale)
{
    const float clampedScale = juce::jlimit(0.25f, 4.0f, scale);
    const float param = (std::log2(clampedScale) / 4.0f) + 0.5f;
    return juce::jlimit(0.0f, 1.0f, param);
}
}  // namespace

juce::String runWowFlutterRateMappingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const std::array<float, 5> params{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const std::array<float, 5> expectedScales{0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    constexpr float kAnchorEps = 1.0e-5f;
    constexpr float kRoundTripEps = 1.0e-6f;

    bool anchorOk = true;
    bool paramRoundTripOk = true;
    float maxAnchorErr = 0.0f;
    float maxParamRoundTripErr = 0.0f;
    for (size_t i = 0; i < params.size(); ++i)
    {
        const float scale = mapRateParamToScale(params[i]);
        const float anchorErr = std::abs(scale - expectedScales[i]);
        maxAnchorErr = std::max(maxAnchorErr, anchorErr);
        if (anchorErr > kAnchorEps)
            anchorOk = false;

        const float roundTripParam = mapRateScaleToParam(scale);
        const float paramErr = std::abs(roundTripParam - params[i]);
        maxParamRoundTripErr = std::max(maxParamRoundTripErr, paramErr);
        if (paramErr > kRoundTripEps)
            paramRoundTripOk = false;
    }

    bool flavorRoundTripOk = true;
    float maxFlavorRoundTripErr = 0.0f;
    int flavorFailures = 0;
    for (int i = 0; i < static_cast<int>(TapeFlavor::NumFlavors); ++i)
    {
        const auto& info = TapeFlavors::get(i);
        const std::array<float, 2> scales{info.wowRateScale, info.flutterRateScale};
        for (float scale : scales)
        {
            const float clampedScale = juce::jlimit(0.25f, 4.0f, scale);
            const float roundTripScale = mapRateParamToScale(mapRateScaleToParam(scale));
            const float scaleErr = std::abs(roundTripScale - clampedScale);
            maxFlavorRoundTripErr = std::max(maxFlavorRoundTripErr, scaleErr);
            if (scaleErr > kRoundTripEps)
            {
                flavorRoundTripOk = false;
                ++flavorFailures;
            }
        }
    }

    const bool passed = anchorOk && paramRoundTripOk && flavorRoundTripOk;
    juce::String result;
    result << "WowFlutterRateMapping: anchorErr=" << juce::String(maxAnchorErr, 7)
           << " paramErr=" << juce::String(maxParamRoundTripErr, 7)
           << " flavorErr=" << juce::String(maxFlavorRoundTripErr, 7) << " flavorFail=" << flavorFailures << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runWowRateTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Measure delay time variations by tracking echo arrival times
    // Wow is slow pitch modulation (0.5-2Hz) from capstan wobble
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int blocksPerDelay = 15;
    const int numImpulses = 10;
    const int ballisticsBlocks = 100;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    // Wait for motor ballistics to settle
    for (int w = 0; w < ballisticsBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    std::vector<float> delayTimesMs;

    for (int imp = 0; imp < numImpulses; ++imp)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float maxPeak = 0.0f;
        int peakSample = 0;
        int sampleCount = blkSize;

        for (int b = 0; b < blocksPerDelay; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float absVal = std::abs(buf[0][static_cast<size_t>(s)]);
                if (absVal > maxPeak && absVal > 0.01f)
                {
                    maxPeak = absVal;
                    peakSample = sampleCount + s;
                }
            }
            sampleCount += blkSize;
        }

        if (maxPeak > 0.01f)
            delayTimesMs.push_back(static_cast<float>(peakSample) / fs * 1000.0f);

        for (int w = 0; w < 10; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }
    }

    float avgDelay = 0.0f, minDelay = 1e9f, maxDelay = 0.0f;
    for (float d : delayTimesMs)
    {
        avgDelay += d;
        minDelay = std::min(minDelay, d);
        maxDelay = std::max(maxDelay, d);
    }
    if (!delayTimesMs.empty())
        avgDelay /= static_cast<float>(delayTimesMs.size());

    float delayVariation = maxDelay - minDelay;
    float variance = 0.0f;
    for (float d : delayTimesMs)
    {
        const float diff = d - avgDelay;
        variance += diff * diff;
    }
    float stdDev =
        (delayTimesMs.size() > 1) ? std::sqrt(variance / (static_cast<float>(delayTimesMs.size() - 1))) : 0.0f;

    bool hasEchoes = delayTimesMs.size() >= 5;
    bool delayReasonable = avgDelay > 100.0f && avgDelay < 150.0f;
    bool hasVariance = stdDev > 0.02f;
    bool passed = hasEchoes && delayReasonable && hasVariance;

    juce::String result;
    result << "Wow: avg=" << juce::String(avgDelay, 1) << "ms, "
           << "var=" << juce::String(delayVariation, 2) << "ms "
           << "std=" << juce::String(stdDev, 3) << "ms "
           << "(" << delayTimesMs.size() << " echoes) " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFlutterRateTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Measure rapid delay variations using multiple impulses
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int blocksPerDelay = 15;
    const int numImpulses = 20;
    const int ballisticsBlocks = 100;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    for (int w = 0; w < ballisticsBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    std::vector<float> delayTimesMs;

    for (int imp = 0; imp < numImpulses; ++imp)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float maxPeak = 0.0f;
        int peakSample = 0;
        int sampleCount = blkSize;

        for (int b = 0; b < blocksPerDelay; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float absVal = std::abs(buf[0][static_cast<size_t>(s)]);
                if (absVal > maxPeak && absVal > 0.01f)
                {
                    maxPeak = absVal;
                    peakSample = sampleCount + s;
                }
            }
            sampleCount += blkSize;
        }

        if (maxPeak > 0.01f)
            delayTimesMs.push_back(static_cast<float>(peakSample) / fs * 1000.0f);

        for (int w = 0; w < 5; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }
    }

    float avgDelay = 0.0f;
    for (float d : delayTimesMs)
        avgDelay += d;
    if (!delayTimesMs.empty())
        avgDelay /= static_cast<float>(delayTimesMs.size());

    int jitterCount = 0;
    float totalJitter = 0.0f;
    for (size_t i = 1; i < delayTimesMs.size(); ++i)
    {
        float diff = std::abs(delayTimesMs[i] - delayTimesMs[i - 1]);
        if (diff > 0.1f)
        {
            jitterCount++;
            totalJitter += diff;
        }
    }

    float avgJitter = jitterCount > 0 ? totalJitter / static_cast<float>(jitterCount) : 0.0f;
    bool hasEchoes = delayTimesMs.size() >= 10;
    bool delayReasonable = avgDelay > 100.0f && avgDelay < 150.0f;
    bool hasJitter = jitterCount > 0 && avgJitter > 0.05f;
    bool passed = hasEchoes && delayReasonable && hasJitter;

    juce::String result;
    result << "Flutter: avg=" << juce::String(avgDelay, 1) << "ms, " << jitterCount << " jitters, "
           << "jit=" << juce::String(avgJitter, 2) << "ms " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runModDepthTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Compare wow/flutter depth via frequency deviation on a 3150 Hz tone.
    const float fs = 48000.0f;
    const int blkSize = 512;
    const float testFreq = 3150.0f;
    const float amp = 0.2f;
    const int warmBlocks = 120;
    const int captureBlocks = 400;
    const int minPeriods = 2000;

    const float depthLow = 0.1f;
    const float depthHigh = 1.0f;

    FrequencyStats lowStats{};
    FrequencyStats highStats{};

    for (int testRun = 0; testRun < 2; ++testRun)
    {
        EchoesModel testModel;
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        const float depth = (testRun == 0) ? depthLow : depthHigh;
        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f,
                                   0.0f,  // noise disabled, pristine tape
                                   depth, 1.0f);

        float phaseA = 0.0f;
        float phaseB = 0.0f;
        processToneBlocks(testModel, buf, blkSize, warmBlocks, fs, testFreq, amp, 0.0f, 0.0f, phaseA, phaseB, nullptr);

        std::vector<float> samples;
        samples.reserve(static_cast<size_t>(captureBlocks) * static_cast<size_t>(blkSize));
        processToneBlocks(testModel, buf, blkSize, captureBlocks, fs, testFreq, amp, 0.0f, 0.0f, phaseA, phaseB,
                          &samples);
        removeDc(samples);
        bandLimitTone(samples, fs);

        FrequencyStats stats = measureZeroCrossingFrequency(samples, fs, testFreq);
        if (testRun == 0)
            lowStats = stats;
        else
            highStats = stats;
    }

    const float ratio = highStats.stdDevHz / std::max(1.0e-6f, lowStats.stdDevHz);
    const bool havePeriods = (lowStats.periods >= minPeriods) && (highStats.periods >= minPeriods);
    const bool meanOk = std::abs(lowStats.meanHz - testFreq) < 60.0f && std::abs(highStats.meanHz - testFreq) < 60.0f;
    const bool depthIncreases = std::isfinite(lowStats.stdDevHz) && std::isfinite(highStats.stdDevHz) &&
                                (highStats.stdDevHz > 0.05f) && (ratio > 1.35f);
    const bool passed = havePeriods && meanOk && depthIncreases;

    juce::String result;
    result << "ModDepth: low=" << juce::String(lowStats.stdDevHz, 3) << "Hz "
           << "high=" << juce::String(highStats.stdDevHz, 3) << "Hz "
           << "ratio=" << juce::String(ratio, 2) << " "
           << "mean=" << juce::String(lowStats.meanHz, 1) << "/" << juce::String(highStats.meanHz, 1) << "Hz "
           << "(" << lowStats.periods << "/" << highStats.periods << " periods) " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runRateChangePitchTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Measure delay time at two different rate settings
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int numMeasurements = 3;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};

    // Test 1: Measure at rate 0.2 (slow tape, long delay)
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.2f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    for (int w = 0; w < ballisticsBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    float avgDelaySlow = 0.0f;
    int countSlow = 0;

    for (int m = 0; m < numMeasurements; ++m)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float maxPeak = 0.0f;
        int peakSample = 0;
        int sampleCount = blkSize;

        for (int b = 0; b < 40; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float absVal = std::abs(buf[0][static_cast<size_t>(s)]);
                if (absVal > maxPeak && absVal > 0.01f)
                {
                    maxPeak = absVal;
                    peakSample = sampleCount + s;
                }
            }
            sampleCount += blkSize;
        }

        if (maxPeak > 0.01f)
        {
            avgDelaySlow += static_cast<float>(peakSample) / fs * 1000.0f;
            countSlow++;
        }

        for (int w = 0; w < 20; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }
    }
    if (countSlow > 0)
        avgDelaySlow /= static_cast<float>(countSlow);

    // Test 2: Measure at rate 0.8 (fast tape, short delay)
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.8f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    for (int w = 0; w < ballisticsBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    float avgDelayFast = 0.0f;
    int countFast = 0;

    for (int m = 0; m < numMeasurements; ++m)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float maxPeak = 0.0f;
        int peakSample = 0;
        int sampleCount = blkSize;

        for (int b = 0; b < 40; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float absVal = std::abs(buf[0][static_cast<size_t>(s)]);
                if (absVal > maxPeak && absVal > 0.01f)
                {
                    maxPeak = absVal;
                    peakSample = sampleCount + s;
                }
            }
            sampleCount += blkSize;
        }

        if (maxPeak > 0.01f)
        {
            avgDelayFast += static_cast<float>(peakSample) / fs * 1000.0f;
            countFast++;
        }

        for (int w = 0; w < 20; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }
    }
    if (countFast > 0)
        avgDelayFast /= static_cast<float>(countFast);

    float delayChange = avgDelaySlow - avgDelayFast;

    // Slow tape should have LONGER delay than fast tape
    bool passed = (countSlow > 0 && countFast > 0 && delayChange > 0);

    juce::String result;
    result << "Rate: @0.2=" << juce::String(avgDelaySlow, 1) << "ms, "
           << "@0.8=" << juce::String(avgDelayFast, 1) << "ms, "
           << "diff=" << juce::String(delayChange, 1) << "ms " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Frequency Response Tests
//==============================================================================

juce::String runSpeedHFTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 50;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float hfEnergySlowTape = 0.0f, hfEnergyFastTape = 0.0f;

    for (int speedTest = 0; speedTest < 2; ++speedTest)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float rate = (speedTest == 0) ? 0.1f : 0.9f;
        testModel.UpdateParameters(0.5f, 0.5f, 0.3f, rate, heads, 1, 1.0f, 0, 1.0f, 1.0f);

        for (int w = 0; w < ballisticsBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float hfEnergy = 0.0f;
        float prevSample = 0.0f;

        for (int b = 0; b < measureBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float diff = buf[0][static_cast<size_t>(s)] - prevSample;
                hfEnergy += diff * diff;
                prevSample = buf[0][static_cast<size_t>(s)];
            }
        }

        if (speedTest == 0)
            hfEnergySlowTape = hfEnergy;
        else
            hfEnergyFastTape = hfEnergy;
    }

    float hfRatio = (hfEnergySlowTape > 0.0001f) ? hfEnergyFastTape / hfEnergySlowTape : 0.0f;

    bool passed = hfRatio > 0.8f;

    juce::String result;
    result << "SpeedHF: slow=" << juce::String(hfEnergySlowTape * 1000.0f, 2) << ", "
           << "fast=" << juce::String(hfEnergyFastTape * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(hfRatio, 2) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSpeedHFBandwidthTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 50;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float hfEnergySlowTape = 0.0f, hfEnergyFastTape = 0.0f;

    for (int speedTest = 0; speedTest < 2; ++speedTest)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float rate = (speedTest == 0) ? 0.1f : 0.9f;
        testModel.UpdateParameters(0.5f, 0.5f, 0.3f, rate, heads, 1, 1.0f, 0, 1.0f, 1.0f);

        for (int w = 0; w < ballisticsBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float hfEnergy = 0.0f;
        float prevSample = 0.0f;

        for (int b = 0; b < measureBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float diff = buf[0][static_cast<size_t>(s)] - prevSample;
                hfEnergy += diff * diff;
                prevSample = buf[0][static_cast<size_t>(s)];
            }
        }

        if (speedTest == 0)
            hfEnergySlowTape = hfEnergy;
        else
            hfEnergyFastTape = hfEnergy;
    }

    float hfRatio = (hfEnergySlowTape > 0.0001f) ? hfEnergyFastTape / hfEnergySlowTape : 0.0f;

    auto measureEchoPathRms = [fs](float delayParam, float frequency) -> float
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 1, blkSize);
        int heads[3] = {1, 0, 0};
        delay.UpdateParameters(0.0f, delayParam, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f,
                               0.0f, 0, 0, true, 0.0f, 0.0f, 0.0f, 1.0f);

        constexpr float amplitude = 0.01f;
        constexpr float renderSeconds = 0.8f;
        constexpr float measureStartSeconds = 0.55f;
        const int totalSamples = static_cast<int>(renderSeconds * fs);
        const int measureStart = static_cast<int>(measureStartSeconds * fs);
        const int totalBlocks = (totalSamples + blkSize - 1) / blkSize;
        const float phaseIncrement = juce::MathConstants<float>::twoPi * frequency / fs;
        float phase = 0.0f;
        std::vector<std::vector<float>> buffer(1, std::vector<float>(blkSize, 0.0f));
        double energy = 0.0;
        int count = 0;

        for (int block = 0; block < totalBlocks; ++block)
        {
            for (int sample = 0; sample < blkSize; ++sample)
            {
                buffer[0][static_cast<size_t>(sample)] = amplitude * std::sin(phase);
                phase += phaseIncrement;
                if (phase >= juce::MathConstants<float>::twoPi)
                    phase -= juce::MathConstants<float>::twoPi;
            }

            delay.ProcessBuffer(buffer, blkSize);
            const int blockStart = block * blkSize;
            for (int sample = 0; sample < blkSize; ++sample)
            {
                const int absoluteSample = blockStart + sample;
                if (absoluteSample < measureStart || absoluteSample >= totalSamples)
                    continue;
                const float value = buffer[0][static_cast<size_t>(sample)];
                energy += static_cast<double>(value) * static_cast<double>(value);
                ++count;
            }
        }

        return count > 0 ? static_cast<float>(std::sqrt(energy / count)) : 0.0f;
    };

    auto measureBandwidth = [&](float delayParam) -> float
    {
        constexpr float referenceFrequency = 1000.0f;
        constexpr float firstFrequency = 1250.0f;
        constexpr float lastFrequency = 20000.0f;
        constexpr float stepHz = 250.0f;
        const float referenceRms = measureEchoPathRms(delayParam, referenceFrequency);
        float previousFrequency = referenceFrequency;
        float previousDb = 0.0f;

        for (float frequency = firstFrequency; frequency <= lastFrequency; frequency += stepHz)
        {
            const float rms = measureEchoPathRms(delayParam, frequency);
            const float responseDb = 20.0f * std::log10(std::max(1.0e-12f, rms) / std::max(1.0e-12f, referenceRms));
            if (responseDb <= -3.0f)
            {
                const float spanDb = responseDb - previousDb;
                const float fraction =
                    std::abs(spanDb) > 1.0e-6f ? juce::jlimit(0.0f, 1.0f, (-3.0f - previousDb) / spanDb) : 0.0f;
                return previousFrequency + fraction * (frequency - previousFrequency);
            }
            previousFrequency = frequency;
            previousDb = responseDb;
        }

        return 0.0f;
    };

    // Coherent single-bin response through the same rendered echo path. RMS
    // over a non-integer cycle count costs ~0.2 dB at 50 Hz, which is the same
    // order as the effect being measured, so the low end is projected instead.
    auto measureEchoPathResponseDb = [fs](float delayParam, float frequency) -> float
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 1, blkSize);
        int heads[3] = {1, 0, 0};
        delay.UpdateParameters(0.0f, delayParam, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f,
                               0.0f, 0, 0, true, 0.0f, 0.0f, 0.0f, 1.0f);

        constexpr float amplitude = 0.01f;
        constexpr float renderSeconds = 2.0f;
        constexpr float measureStartSeconds = 1.0f;
        const int totalSamples = static_cast<int>(renderSeconds * fs);
        const int measureStart = static_cast<int>(measureStartSeconds * fs);
        const int totalBlocks = (totalSamples + blkSize - 1) / blkSize;
        const double phaseIncrement = juce::MathConstants<double>::twoPi * static_cast<double>(frequency) / fs;
        double phase = 0.0;
        double measurePhase = 0.0;
        std::vector<std::vector<float>> buffer(1, std::vector<float>(blkSize, 0.0f));
        double sinProjection = 0.0;
        double cosProjection = 0.0;
        int count = 0;

        for (int block = 0; block < totalBlocks; ++block)
        {
            for (int sample = 0; sample < blkSize; ++sample)
            {
                buffer[0][static_cast<size_t>(sample)] = amplitude * static_cast<float>(std::sin(phase));
                phase += phaseIncrement;
                if (phase >= juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
            }

            delay.ProcessBuffer(buffer, blkSize);
            const int blockStart = block * blkSize;
            for (int sample = 0; sample < blkSize; ++sample)
            {
                const int absoluteSample = blockStart + sample;
                if (absoluteSample < measureStart || absoluteSample >= totalSamples)
                    continue;
                const double value = static_cast<double>(buffer[0][static_cast<size_t>(sample)]);
                sinProjection += value * std::sin(measurePhase);
                cosProjection += value * std::cos(measurePhase);
                measurePhase += phaseIncrement;
                if (measurePhase >= juce::MathConstants<double>::twoPi)
                    measurePhase -= juce::MathConstants<double>::twoPi;
                ++count;
            }
        }

        if (count <= 0)
            return -240.0f;
        const double magnitude = 2.0 * std::hypot(sinProjection, cosProjection) / static_cast<double>(count);
        return 20.0f * std::log10(static_cast<float>(std::max(1.0e-12, magnitude / static_cast<double>(amplitude))));
    };

    // Playback-head low end. A tape reproduce chain (NAB 3180 us / IEC 3180 us
    // low-frequency time constant, i.e. a 50 Hz corner, and machine specs that
    // quote 30-40 Hz at the low end) is flat well below 100 Hz. A repeat that
    // is several dB down at 100 Hz cannot carry bass, so this is measured, not
    // assumed.
    const float lowEndReferenceDb = measureEchoPathResponseDb(1.0f, 1000.0f);
    const float lowEnd50Db = measureEchoPathResponseDb(1.0f, 50.0f) - lowEndReferenceDb;
    const float lowEnd100Db = measureEchoPathResponseDb(1.0f, 100.0f) - lowEndReferenceDb;
    const float lowEnd200Db = measureEchoPathResponseDb(1.0f, 200.0f) - lowEndReferenceDb;
    float lowCornerHz = 0.0f;
    {
        float previousFrequency = 20.0f;
        float previousDb = measureEchoPathResponseDb(1.0f, 20.0f) - lowEndReferenceDb;
        for (float frequency = 25.0f; frequency <= 400.0f; frequency += 5.0f)
        {
            const float responseDb = measureEchoPathResponseDb(1.0f, frequency) - lowEndReferenceDb;
            if (responseDb >= -3.0f)
            {
                const float spanDb = responseDb - previousDb;
                const float fraction =
                    std::abs(spanDb) > 1.0e-6f ? juce::jlimit(0.0f, 1.0f, (-3.0f - previousDb) / spanDb) : 0.0f;
                lowCornerHz = previousFrequency + fraction * (frequency - previousFrequency);
                break;
            }
            previousFrequency = frequency;
            previousDb = responseDb;
        }
    }

    const float bandwidth70Ms = measureBandwidth(1.0f);
    const float bandwidth175Ms = measureBandwidth(0.0f);
    // Speed contour, expressed against the CONFIGURED playback cutoffs rather
    // than as hardcoded Hz.
    //
    // The previous form pinned bw175 to 5000-8000 Hz. That band was written
    // when PLAYBACK_SLOW_CUTOFF_HZ was 10 kHz; the next voicing commit raised
    // it to 14 kHz and never touched the band, so the gate has been asserting
    // a tone law the DSP was deliberately moved away from. Deriving the band
    // from the shipping constants makes the same rot impossible: the test now
    // checks that the RENDERED bandwidth tracks the cutoff the playback filter
    // was actually given, which is the property worth defending.
    //
    // Physics: shorter delay = faster tape = wider bandwidth. Real machines
    // scale HF bandwidth roughly with speed (gap loss is a wavelength effect),
    // and the 70 ms / 175 ms pair is a 2.5:1 speed ratio. This plugin
    // deliberately renders a gentler contour than 2.5:1, so the ordering and
    // its minimum margin are asserted rather than the physical exponent.
    const float configuredFastCutoffHz = TapeDelayProcessor::PLAYBACK_FAST_CUTOFF_HZ;
    const float configuredSlowCutoffHz = TapeDelayProcessor::PLAYBACK_SLOW_CUTOFF_HZ;
    const float renderedFastFraction = bandwidth70Ms / configuredFastCutoffHz;
    const float renderedSlowFraction = bandwidth175Ms / configuredSlowCutoffHz;
    // Below 0.55 the rest of the path is swallowing the playback filter (a
    // uniformly dark response measures 0.49); above 0.80 the playback filter
    // is not in circuit at all.
    const bool bandwidth70Ok = renderedFastFraction >= 0.55f && renderedFastFraction <= 0.80f;
    const bool bandwidth175Ok = renderedSlowFraction >= 0.55f && renderedSlowFraction <= 0.80f;
    const float speedContourRatio = bandwidth175Ms > 1.0f ? bandwidth70Ms / bandwidth175Ms : 0.0f;
    const bool speedContourOk = speedContourRatio >= 1.20f;

    // Playback low end. A tape reproduce chain is flat well below 100 Hz; the
    // shipped Q=0.25 turned a nominal 40 Hz high-pass into a first-order
    // ~149 Hz one and the rendered echo measured a 167 Hz corner with -6.2 dB
    // at 100 Hz. The corner is allowed to sit above the configured playback
    // corner (the record-path 60 Hz one-pole is in series) but not to be a
    // different filter from the one the code asks for.
    const float configuredLowCornerHz = TapeDelayProcessor::PLAYBACK_HP_CUTOFF_HZ;
    const bool lowCornerOk = lowCornerHz >= configuredLowCornerHz && lowCornerHz <= 2.2f * configuredLowCornerHz;
    const bool lowEnd100Ok = lowEnd100Db >= -2.5f;
    const bool lowEnd200Ok = lowEnd200Db >= -1.0f;
    const bool lowEndOk = lowCornerOk && lowEnd100Ok && lowEnd200Ok;

    bool passed = hfRatio > 0.8f && bandwidth70Ok && bandwidth175Ok && speedContourOk && lowEndOk;

    juce::String result;
    result << "SpeedHFBandwidth: slow=" << juce::String(hfEnergySlowTape * 1000.0f, 2) << ", "
           << "fast=" << juce::String(hfEnergyFastTape * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(hfRatio, 2) << ", "
           << "bw70=" << juce::String(bandwidth70Ms, 0) << "Hz, "
           << "bw175=" << juce::String(bandwidth175Ms, 0) << "Hz, "
           << "lf50=" << juce::String(lowEnd50Db, 2) << "dB, "
           << "lf100=" << juce::String(lowEnd100Db, 2) << "dB, "
           << "lf200=" << juce::String(lowEnd200Db, 2) << "dB, "
           << "lfCorner=" << juce::String(lowCornerHz, 1) << "Hz, "
           << "renderedFrac=" << juce::String(renderedFastFraction, 3) << "/" << juce::String(renderedSlowFraction, 3)
           << " band=[0.55,0.80], "
           << "speedRatio=" << juce::String(speedContourRatio, 3) << " min=1.20, "
           << "lfCornerBand=[" << juce::String(configuredLowCornerHz, 0) << ","
           << juce::String(2.2f * configuredLowCornerHz, 0) << "]Hz " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runConditionDarkTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 50;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float hfEnergyPristine = 0.0f, hfEnergyWorn = 0.0f;

    for (int condTest = 0; condTest < 2; ++condTest)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float condition = (condTest == 0) ? 0.0f : 1.0f;
        testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, condition, 0.0f,
                                   1.0f, 1.0f);

        for (int w = 0; w < ballisticsBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float hfEnergy = 0.0f;
        float prevSample = 0.0f;

        for (int b = 0; b < measureBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
            {
                float diff = buf[0][static_cast<size_t>(s)] - prevSample;
                hfEnergy += diff * diff;
                prevSample = buf[0][static_cast<size_t>(s)];
            }
        }

        if (condTest == 0)
            hfEnergyPristine = hfEnergy;
        else
            hfEnergyWorn = hfEnergy;
    }

    float hfRatio = (hfEnergyPristine > 0.0001f) ? hfEnergyWorn / hfEnergyPristine : 0.0f;

    bool passed = hfRatio < 1.2f;

    juce::String result;
    result << "Condition: pristine=" << juce::String(hfEnergyPristine * 1000.0f, 2) << ", "
           << "worn=" << juce::String(hfEnergyWorn * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(hfRatio, 2) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runRecordChainTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    for (int w = 0; w < ballisticsBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    float inputPeak = 0.9f;
    std::fill(buf[0].begin(), buf[0].end(), 0.0f);
    std::fill(buf[1].begin(), buf[1].end(), 0.0f);
    buf[0][0] = inputPeak;
    buf[1][0] = inputPeak;
    testModel.ProcessBuffer(buf, blkSize);

    float echoPeak = 0.0f;
    for (int b = 0; b < 20; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        for (int s = 0; s < blkSize; ++s)
            echoPeak = std::max(echoPeak, std::abs(buf[0][static_cast<size_t>(s)]));
    }

    float attenuation = (inputPeak > 0) ? echoPeak / inputPeak : 0.0f;
    float attenDb = 20.0f * std::log10(std::max(0.0001f, attenuation));

    bool passed = echoPeak > 0.01f && attenDb < 0.0f;

    juce::String result;
    result << "RecordChain: in=" << juce::String(inputPeak, 2) << ", "
           << "echo=" << juce::String(echoPeak, 3) << ", "
           << "atten=" << juce::String(attenDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPlaybackEQTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 30;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float energyLowEQ = 0.0f, energyHighEQ = 0.0f;

    for (int eqTest = 0; eqTest < 2; ++eqTest)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float low = (eqTest == 0) ? 0.0f : 1.0f;
        float high = (eqTest == 0) ? 0.0f : 1.0f;
        testModel.UpdateParameters(low, high, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

        for (int w = 0; w < ballisticsBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        float energy = 0.0f;
        for (int b = 0; b < measureBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
                energy += buf[0][static_cast<size_t>(s)] * buf[0][static_cast<size_t>(s)];
        }

        if (eqTest == 0)
            energyLowEQ = energy;
        else
            energyHighEQ = energy;
    }

    float energyRatio = (energyLowEQ > 0.0001f) ? energyHighEQ / energyLowEQ : 0.0f;

    bool passed = energyLowEQ > 0.0001f && energyHighEQ > 0.0001f;

    juce::String result;
    result << "PlaybackEQ: lowEQ=" << juce::String(energyLowEQ * 1000.0f, 2) << ", "
           << "highEQ=" << juce::String(energyHighEQ * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(energyRatio, 2) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runTapeAliasingImdTest()
{
    const int fftSize = 8192;
    const int blkSize = 512;
    const int warmBlocks = 200;
    const int captureBlocks = fftSize / blkSize;
    const int harmonicCount = 6;
    const float intensity = 1.0f;
    const float noFeedbackIntensity = 0.0f;
    const float tapeDrive = 1.0f;
    const float tapeSaturation = 1.0f;
    const float tapeBias = 0.5f;
    const float aliasFreqTarget = 8000.0f;
    const float imdFreqATarget = 5000.0f;
    const float imdFreqBTarget = 6000.0f;
    const float ampSingle = 0.12f;
    const float ampTone = 0.08f;
    const float minFundMag = 1.0e-5f;

    std::array<float, 3> sampleRates{44100.0f, 48000.0f, 96000.0f};
    float aliasDb441 = 0.0f, aliasDb480 = 0.0f, aliasDb960 = 0.0f;
    float aliasAbsDb441 = 0.0f, aliasAbsDb480 = 0.0f, aliasAbsDb960 = 0.0f;
    float imdDb441 = 0.0f, imdDb480 = 0.0f, imdDb960 = 0.0f;
    float recordImdDb441 = 0.0f, recordImdDb480 = 0.0f, recordImdDb960 = 0.0f;
    float noFbImdDb441 = 0.0f, noFbImdDb480 = 0.0f, noFbImdDb960 = 0.0f;
    float playImdDb441 = 0.0f, playImdDb480 = 0.0f, playImdDb960 = 0.0f;
    float preampImdDb441 = 0.0f, preampImdDb480 = 0.0f, preampImdDb960 = 0.0f;
    float delayOnlyImdDb441 = 0.0f, delayOnlyImdDb480 = 0.0f, delayOnlyImdDb960 = 0.0f;
    float delayOnlyNoFbImdDb441 = 0.0f, delayOnlyNoFbImdDb480 = 0.0f, delayOnlyNoFbImdDb960 = 0.0f;
    float aliasFreq441 = 0.0f, aliasFreq480 = 0.0f, aliasFreq960 = 0.0f;
    float imdFreqA441 = 0.0f, imdFreqB441 = 0.0f;
    float imdFreqA480 = 0.0f, imdFreqB480 = 0.0f;
    float imdFreqA960 = 0.0f, imdFreqB960 = 0.0f;
    double estimatorErrorDb = 0.0;
    const bool estimatorQualified = qualifyAliasEstimator(estimatorErrorDb);
    bool passed = estimatorQualified;

    for (float fs : sampleRates)
    {
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        int heads[3] = {1, 0, 0};
        const float binFreq = fs / static_cast<float>(fftSize);
        const float aliasFreq = alignFrequencyToBin(aliasFreqTarget, fs, fftSize);
        float imdFreqA = alignFrequencyToBin(imdFreqATarget, fs, fftSize);
        float imdFreqB = alignFrequencyToBin(imdFreqBTarget, fs, fftSize);
        if (std::abs(imdFreqB - imdFreqA) < binFreq)
            imdFreqB = std::min((fs * 0.5f) - binFreq, imdFreqB + binFreq);

        // Aliasing measurement (single tone)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            configureStressModel(model, heads, intensity, tapeDrive, tapeSaturation, tapeBias);

            float phase = 0.0f;
            processRecordChainBlocks(model, buf, blkSize, warmBlocks, fs, aliasFreq, ampSingle, phase, nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processRecordChainBlocks(model, buf, blkSize, captureBlocks, fs, aliasFreq, ampSingle, phase, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);
            const float aliasDb = computeAliasRatioDb(spectrum, fs, aliasFreq, harmonicCount);
            const float aliasAbsDb = computeAliasToTotalDb(spectrum, fs, aliasFreq, harmonicCount);
            const float fundMag = magnitudeAt(spectrum, fs, aliasFreq);

            const bool aliasValid = fundMag > minFundMag && std::isfinite(aliasDb) && std::isfinite(aliasAbsDb);
            const bool aliasReleaseOk = aliasDb <= kAliasRatioReleaseDb && aliasAbsDb <= kAliasAbsReleaseDb;

            if (!aliasValid || !aliasReleaseOk)
                passed = false;

            if (fs < 47000.0f)
            {
                aliasDb441 = aliasDb;
                aliasAbsDb441 = aliasAbsDb;
                aliasFreq441 = aliasFreq;
            }
            else if (fs < 70000.0f)
            {
                aliasDb480 = aliasDb;
                aliasAbsDb480 = aliasAbsDb;
                aliasFreq480 = aliasFreq;
            }
            else
            {
                aliasDb960 = aliasDb;
                aliasAbsDb960 = aliasAbsDb;
                aliasFreq960 = aliasFreq;
            }
        }

        // IMD measurement (two tone)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            configureStressModel(model, heads, intensity, tapeDrive, tapeSaturation, tapeBias);

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processToneBlocks(model, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone, phaseA, phaseB,
                              nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processToneBlocks(model, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone, phaseA,
                              phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            const float imdDb =
                20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));

            if (fs < 47000.0f)
            {
                imdDb441 = imdDb;
                imdFreqA441 = imdFreqA;
                imdFreqB441 = imdFreqB;
            }
            else if (fs < 70000.0f)
            {
                imdDb480 = imdDb;
                imdFreqA480 = imdFreqA;
                imdFreqB480 = imdFreqB;
            }
            else
            {
                imdDb960 = imdDb;
                imdFreqA960 = imdFreqA;
                imdFreqB960 = imdFreqB;
            }
        }

        // IMD measurement (preamp-only)
        {
            PreampProcessor preamp;
            preamp.reset(fs, 2, blkSize);
            preamp.updateParameters(1.0f);  // unity input level (matches stress model)

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processToneBlocksPreamp(preamp, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone, phaseA,
                                    phaseB, nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processToneBlocksPreamp(preamp, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                    phaseA, phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            float preImdDb = std::numeric_limits<float>::quiet_NaN();
            if (fundMag > minFundMag)
            {
                preImdDb =
                    20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
            }

            if (fs < 47000.0f)
                preampImdDb441 = preImdDb;
            else if (fs < 70000.0f)
                preampImdDb480 = preImdDb;
            else
                preampImdDb960 = preImdDb;
        }

        // IMD measurement (delay-only, no preamp; includes record + playback)
        {
            TapeDelayProcessor delay;
            delay.Reset(fs, 2);
            delay.setTapeCharacter(tapeDrive, tapeSaturation, tapeBias);
            delay.setTapeRecordMakeupGainDb(0.0f);

            int headsOnly[3] = {1, 0, 0};
            delay.UpdateParameters(intensity, 0.5f, headsOnly, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false,
                                   0.0f, 0.0f, 0.0f);

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processToneBlocksDelayOnly(delay, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                       phaseA, phaseB, nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processToneBlocksDelayOnly(delay, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                       phaseA, phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            float tdImdDb = std::numeric_limits<float>::quiet_NaN();
            if (fundMag > minFundMag)
            {
                tdImdDb =
                    20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
            }

            if (fs < 47000.0f)
                delayOnlyImdDb441 = tdImdDb;
            else if (fs < 70000.0f)
                delayOnlyImdDb480 = tdImdDb;
            else
                delayOnlyImdDb960 = tdImdDb;
        }

        // IMD measurement (delay-only, no preamp, no feedback)
        {
            TapeDelayProcessor delay;
            delay.Reset(fs, 2);
            delay.setTapeCharacter(tapeDrive, tapeSaturation, tapeBias);
            delay.setTapeRecordMakeupGainDb(0.0f);

            int headsOnly[3] = {1, 0, 0};
            delay.UpdateParameters(noFeedbackIntensity, 0.5f, headsOnly, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                   false, 0.0f, 0.0f, 0.0f);

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processToneBlocksDelayOnly(delay, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                       phaseA, phaseB, nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processToneBlocksDelayOnly(delay, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                       phaseA, phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            float tdImdDb = std::numeric_limits<float>::quiet_NaN();
            if (fundMag > minFundMag)
            {
                tdImdDb =
                    20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
            }

            if (fs < 47000.0f)
                delayOnlyNoFbImdDb441 = tdImdDb;
            else if (fs < 70000.0f)
                delayOnlyNoFbImdDb480 = tdImdDb;
            else
                delayOnlyNoFbImdDb960 = tdImdDb;
        }

        // IMD measurement (two tone, no feedback)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            configureStressModel(model, heads, noFeedbackIntensity, tapeDrive, tapeSaturation, tapeBias);

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processToneBlocks(model, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone, phaseA, phaseB,
                              nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processToneBlocks(model, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone, phaseA,
                              phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            float noFbImdDb = std::numeric_limits<float>::quiet_NaN();
            if (fundMag > minFundMag)
            {
                noFbImdDb =
                    20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
            }

            const bool imdNoFbValid = fundMag > minFundMag && std::isfinite(noFbImdDb);
            const bool imdNoFbReleaseOk = imdNoFbValid && (noFbImdDb <= kImdReleaseDb);
            if (!imdNoFbReleaseOk)
                passed = false;

            if (fs < 47000.0f)
                noFbImdDb441 = noFbImdDb;
            else if (fs < 70000.0f)
                noFbImdDb480 = noFbImdDb;
            else
                noFbImdDb960 = noFbImdDb;
        }

        // Record-chain-only IMD measurement (two tone)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            configureStressModel(model, heads, intensity, tapeDrive, tapeSaturation, tapeBias);

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processRecordChainToneBlocks(model, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                         phaseA, phaseB, nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processRecordChainToneBlocks(model, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                         phaseA, phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            float recordImdDb = 0.0f;
            if (fundMag > minFundMag)
            {
                recordImdDb =
                    20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
            }

            if (fs < 47000.0f)
                recordImdDb441 = recordImdDb;
            else if (fs < 70000.0f)
                recordImdDb480 = recordImdDb;
            else
                recordImdDb960 = recordImdDb;
        }

        // Playback-only IMD measurement (two tone, no record chain)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            configureStressModel(model, heads, noFeedbackIntensity, tapeDrive, tapeSaturation, tapeBias);

            float phaseA = 0.0f;
            float phaseB = 0.0f;
            processPlaybackOnlyToneBlocks(model, buf, blkSize, warmBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                          phaseA, phaseB, nullptr);

            std::vector<float> samples;
            samples.reserve(fftSize);
            processPlaybackOnlyToneBlocks(model, buf, blkSize, captureBlocks, fs, imdFreqA, ampTone, imdFreqB, ampTone,
                                          phaseA, phaseB, &samples);
            samples.resize(fftSize);
            removeDc(samples);

            std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
            computeSpectrum(samples, spectrum);

            const float magA = magnitudeAt(spectrum, fs, imdFreqA);
            const float magB = magnitudeAt(spectrum, fs, imdFreqB);
            const float fundMag = std::max(magA, magB);
            const float magImd = magnitudeAt(spectrum, fs, std::abs(imdFreqB - imdFreqA));

            float playImdDb = 0.0f;
            if (fundMag > minFundMag)
            {
                playImdDb =
                    20.0f * std::log10(std::max(1.0e-12f, magImd)) - 20.0f * std::log10(std::max(1.0e-12f, fundMag));
            }

            if (fs < 47000.0f)
                playImdDb441 = playImdDb;
            else if (fs < 70000.0f)
                playImdDb480 = playImdDb;
            else
                playImdDb960 = playImdDb;
        }
    }

    juce::StringArray warningReasons;
    if ((aliasDb441 > kAliasRatioHqDb) || (aliasDb480 > kAliasRatioHqDb) || (aliasDb960 > kAliasRatioHqDb))
        warningReasons.add("alias-ratio");
    if ((aliasAbsDb441 > kAliasAbsHqDb) || (aliasAbsDb480 > kAliasAbsHqDb) || (aliasAbsDb960 > kAliasAbsHqDb))
        warningReasons.add("alias-abs");
    if ((preampImdDb441 > kImdHqDb) || (preampImdDb480 > kImdHqDb) || (preampImdDb960 > kImdHqDb) ||
        (delayOnlyNoFbImdDb441 > kImdHqDb) || (delayOnlyNoFbImdDb480 > kImdHqDb) || (delayOnlyNoFbImdDb960 > kImdHqDb))
        warningReasons.add("imd-no-feedback");
    if ((imdDb441 > kImdHqDb) || (imdDb480 > kImdHqDb) || (imdDb960 > kImdHqDb))
        warningReasons.add("imd-feedback");
    const bool warnHigh = !warningReasons.isEmpty();

    juce::String result;
    result << "TapeAliasingIMD: "
           << "estimatorControls=" << (estimatorQualified ? "yes" : "no")
           << " estimatorError=" << juce::String(estimatorErrorDb, 6) << "dB "
           << "44100Hz alias=" << juce::String(aliasDb441, 1) << "dB "
           << "abs=" << juce::String(aliasAbsDb441, 1) << "dB "
           << "imdFb=" << juce::String(imdDb441, 1) << "dB "
           << "recImd=" << juce::String(recordImdDb441, 1) << "dB "
           << "nofbImd=" << juce::String(noFbImdDb441, 1) << "dB "
           << "playImd=" << juce::String(playImdDb441, 1) << "dB "
           << "preImd=" << juce::String(preampImdDb441, 1) << "dB "
           << "tdImd=" << juce::String(delayOnlyImdDb441, 1) << "dB "
           << "tdNoFbImd=" << juce::String(delayOnlyNoFbImdDb441, 1) << "dB "
           << "tone=" << juce::String(aliasFreq441, 1) << "Hz "
           << "imd=" << juce::String(imdFreqA441, 1) << "/" << juce::String(imdFreqB441, 1) << "Hz "
           << "48000Hz alias=" << juce::String(aliasDb480, 1) << "dB "
           << "abs=" << juce::String(aliasAbsDb480, 1) << "dB "
           << "imdFb=" << juce::String(imdDb480, 1) << "dB "
           << "recImd=" << juce::String(recordImdDb480, 1) << "dB "
           << "nofbImd=" << juce::String(noFbImdDb480, 1) << "dB "
           << "playImd=" << juce::String(playImdDb480, 1) << "dB "
           << "preImd=" << juce::String(preampImdDb480, 1) << "dB "
           << "tdImd=" << juce::String(delayOnlyImdDb480, 1) << "dB "
           << "tdNoFbImd=" << juce::String(delayOnlyNoFbImdDb480, 1) << "dB "
           << "tone=" << juce::String(aliasFreq480, 1) << "Hz "
           << "imd=" << juce::String(imdFreqA480, 1) << "/" << juce::String(imdFreqB480, 1) << "Hz "
           << "96000Hz alias=" << juce::String(aliasDb960, 1) << "dB "
           << "abs=" << juce::String(aliasAbsDb960, 1) << "dB "
           << "imdFb=" << juce::String(imdDb960, 1) << "dB "
           << "recImd=" << juce::String(recordImdDb960, 1) << "dB "
           << "nofbImd=" << juce::String(noFbImdDb960, 1) << "dB "
           << "playImd=" << juce::String(playImdDb960, 1) << "dB "
           << "preImd=" << juce::String(preampImdDb960, 1) << "dB "
           << "tdImd=" << juce::String(delayOnlyImdDb960, 1) << "dB "
           << "tdNoFbImd=" << juce::String(delayOnlyNoFbImdDb960, 1) << "dB "
           << "tone=" << juce::String(aliasFreq960, 1) << "Hz "
           << "imd=" << juce::String(imdFreqA960, 1) << "/" << juce::String(imdFreqB960, 1) << "Hz "
           << "target alias<=" << juce::String(kAliasRatioReleaseDb, 0) << "dB "
           << "abs<=" << juce::String(kAliasAbsReleaseDb, 0) << "dB "
           << "imdNoFb<=" << juce::String(kImdReleaseDb, 0) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    if (passed && warnHigh)
        result << " warn=high(" << warningReasons.joinIntoString(",") << ")";
    return result;
}

juce::String runTapeNoFeedbackImdBudgetTest()
{
    const int fftSize = 8192;
    const int blkSize = 512;
    const int warmBlocks = 200;
    const float imdFreqATarget = 5000.0f;
    const float imdFreqBTarget = 6000.0f;
    const float ampTone = 0.08f;
    const float minFundMag = 1.0e-5f;

    std::array<float, 3> sampleRates{44100.0f, 48000.0f, 96000.0f};
    std::array<float, 3> postEqImdDb{};
    std::array<float, 3> preampImdDb{};
    std::array<float, 3> delayNoFbImdDb{};
    std::array<float, 3> freqA{};
    std::array<float, 3> freqB{};

    bool passed = true;
    for (size_t i = 0; i < sampleRates.size(); ++i)
    {
        const float fs = sampleRates[i];
        const float binFreq = fs / static_cast<float>(fftSize);
        freqA[i] = alignFrequencyToBin(imdFreqATarget, fs, fftSize);
        freqB[i] = alignFrequencyToBin(imdFreqBTarget, fs, fftSize);
        if (std::abs(freqB[i] - freqA[i]) < binFreq)
            freqB[i] = std::min((fs * 0.5f) - binFreq, freqB[i] + binFreq);

        postEqImdDb[i] =
            measureFullNoFeedbackImd(fs, fftSize, blkSize, warmBlocks, freqA[i], freqB[i], ampTone, minFundMag);
        preampImdDb[i] =
            measurePreampOnlyImd(fs, fftSize, blkSize, warmBlocks, freqA[i], freqB[i], ampTone, minFundMag);
        delayNoFbImdDb[i] =
            measureDelayOnlyNoFeedbackImd(fs, fftSize, blkSize, warmBlocks, freqA[i], freqB[i], ampTone, minFundMag);

        // The post-EQ no-feedback ratio is diagnostic only: intentional tape/tone
        // filtering attenuates 5/6 kHz fundamentals more than the 1 kHz difference product.
        const bool generatedStagesOk = std::isfinite(preampImdDb[i]) && std::isfinite(delayNoFbImdDb[i]) &&
                                       preampImdDb[i] <= kImdHqDb && delayNoFbImdDb[i] <= kImdHqDb;
        if (!generatedStagesOk)
            passed = false;
    }

    juce::String result;
    result << "TapeNoFeedbackIMD: "
           << "44100Hz postEq=" << juce::String(postEqImdDb[0], 1) << "dB "
           << "preamp=" << juce::String(preampImdDb[0], 1) << "dB "
           << "delayNoFb=" << juce::String(delayNoFbImdDb[0], 1) << "dB "
           << "tone=" << juce::String(freqA[0], 1) << "/" << juce::String(freqB[0], 1) << "Hz "
           << "48000Hz postEq=" << juce::String(postEqImdDb[1], 1) << "dB "
           << "preamp=" << juce::String(preampImdDb[1], 1) << "dB "
           << "delayNoFb=" << juce::String(delayNoFbImdDb[1], 1) << "dB "
           << "tone=" << juce::String(freqA[1], 1) << "/" << juce::String(freqB[1], 1) << "Hz "
           << "96000Hz postEq=" << juce::String(postEqImdDb[2], 1) << "dB "
           << "preamp=" << juce::String(preampImdDb[2], 1) << "dB "
           << "delayNoFb=" << juce::String(delayNoFbImdDb[2], 1) << "dB "
           << "tone=" << juce::String(freqA[2], 1) << "/" << juce::String(freqB[2], 1) << "Hz "
           << "generatedTarget<=" << juce::String(kImdHqDb, 0) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runRecordChainAliasTest()
{
    const int fftSize = 8192;
    const int blkSize = 512;
    const int warmBlocks = 100;
    const int captureBlocks = fftSize / blkSize;
    const int harmonicCount = 6;
    const float intensity = 0.0f;
    const float tapeDrive = 1.0f;
    const float tapeSaturation = 1.0f;
    const float tapeBias = 0.5f;
    const float testFreqTarget = 10000.0f;
    const float amp = 0.2f;
    const float minFundMag = 1.0e-5f;

    std::array<float, 3> sampleRates{44100.0f, 48000.0f, 96000.0f};
    float aliasDb441 = 0.0f, aliasDb480 = 0.0f, aliasDb960 = 0.0f;
    float aliasAbsDb441 = 0.0f, aliasAbsDb480 = 0.0f, aliasAbsDb960 = 0.0f;
    float testFreq441 = 0.0f, testFreq480 = 0.0f, testFreq960 = 0.0f;
    double estimatorErrorDb = 0.0;
    const bool estimatorQualified = qualifyAliasEstimator(estimatorErrorDb);
    bool passed = estimatorQualified;

    for (float fs : sampleRates)
    {
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        int heads[3] = {1, 0, 0};
        const float testFreq = alignFrequencyToBin(testFreqTarget, fs, fftSize);

        EchoesModel model;
        model.Reset(fs, 2, blkSize);
        model.UpdateParameters(0.5f, 0.5f, intensity, 1.0f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2,
                               tapeDrive, tapeSaturation, tapeBias, 0, 0);

        float phase = 0.0f;
        processRecordChainBlocks(model, buf, blkSize, warmBlocks, fs, testFreq, amp, phase, nullptr);

        std::vector<float> samples;
        samples.reserve(fftSize);
        processRecordChainBlocks(model, buf, blkSize, captureBlocks, fs, testFreq, amp, phase, &samples);
        samples.resize(fftSize);
        removeDc(samples);

        std::vector<std::complex<float>> spectrum(fftSize / 2 + 1);
        computeSpectrum(samples, spectrum);
        const float aliasDb = computeAliasRatioDb(spectrum, fs, testFreq, harmonicCount);
        const float aliasAbsDb = computeAliasToTotalDb(spectrum, fs, testFreq, harmonicCount);
        const float fundMag = magnitudeAt(spectrum, fs, testFreq);

        const bool aliasValid = fundMag > minFundMag && std::isfinite(aliasDb) && std::isfinite(aliasAbsDb);
        const bool aliasReleaseOk = aliasDb <= kAliasRatioReleaseDb && aliasAbsDb <= kAliasAbsReleaseDb;

        if (!aliasValid || !aliasReleaseOk)
            passed = false;

        if (fs < 47000.0f)
        {
            aliasDb441 = aliasDb;
            aliasAbsDb441 = aliasAbsDb;
            testFreq441 = testFreq;
        }
        else if (fs < 70000.0f)
        {
            aliasDb480 = aliasDb;
            aliasAbsDb480 = aliasAbsDb;
            testFreq480 = testFreq;
        }
        else
        {
            aliasDb960 = aliasDb;
            aliasAbsDb960 = aliasAbsDb;
            testFreq960 = testFreq;
        }
    }

    const bool warnHigh = (aliasDb441 > kAliasRatioHqDb) || (aliasDb480 > kAliasRatioHqDb) ||
                          (aliasDb960 > kAliasRatioHqDb) || (aliasAbsDb441 > kAliasAbsHqDb) ||
                          (aliasAbsDb480 > kAliasAbsHqDb) || (aliasAbsDb960 > kAliasAbsHqDb);

    juce::String result;
    result << "RecordChainAlias: "
           << "estimatorControls=" << (estimatorQualified ? "yes" : "no")
           << " estimatorError=" << juce::String(estimatorErrorDb, 6) << "dB "
           << "44100Hz alias=" << juce::String(aliasDb441, 1) << "dB "
           << "abs=" << juce::String(aliasAbsDb441, 1) << "dB "
           << "tone=" << juce::String(testFreq441, 1) << "Hz "
           << "48000Hz alias=" << juce::String(aliasDb480, 1) << "dB "
           << "abs=" << juce::String(aliasAbsDb480, 1) << "dB "
           << "tone=" << juce::String(testFreq480, 1) << "Hz "
           << "96000Hz alias=" << juce::String(aliasDb960, 1) << "dB "
           << "abs=" << juce::String(aliasAbsDb960, 1) << "dB "
           << "tone=" << juce::String(testFreq960, 1) << "Hz "
           << "target alias<=" << juce::String(kAliasRatioReleaseDb, 0) << "dB "
           << "abs<=" << juce::String(kAliasAbsReleaseDb, 0) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    if (passed && warnHigh)
        result << " warn=high";
    return result;
}

}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
