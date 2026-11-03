// TapeChainBaseline: Deterministic, machine-readable tape-chain metrics (THD-vs-drive, harmonic profiles, wow/flutter
//   sidebands, hiss floor, per-flavor frequency response, self-oscillation, regeneration darkening). JSON written to the
//   artifact dir.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "TapeTests.h"
#include "../../DSP/EchoesModel.h"
#include "../../DSP/TapeNoiseProcessor.h"
#include "../../DSP/TapeFlavors.h"
#include "../../DSP/OouraFFT.h"
#include "../Common/TestUtilities.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <utility>
#include <vector>

namespace SpatialEchoTests::TapeEmulation
{

namespace baseline
{

static juce::File artifactDirectory()
{
    auto dir = SpatialEchoTests::artifactDirectory().getChildFile("tape-baseline");
    dir.createDirectory();
    return dir;
}

// Quantize for byte-identical JSON across runs.
static double r2(double v)
{
    if (!std::isfinite(v))
        return -999.0;
    return std::round(v * 100.0) / 100.0;
}

static float alignFrequencyToBin(float targetFreq, float fs, int fftSize)
{
    const float binFreq = fs / static_cast<float>(fftSize);
    int bin = static_cast<int>(std::round(targetFreq / binFreq));
    bin = juce::jlimit(1, fftSize / 2 - 1, bin);
    return static_cast<float>(bin) * binFreq;
}

static void computeSpectrum(const std::vector<float>& samples, std::vector<std::complex<float>>& spectrum)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    std::vector<float> in(static_cast<size_t>(fftSize), 0.0f);
    for (int i = 0; i < fftSize && i < static_cast<int>(samples.size()); ++i)
    {
        const float w = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) /
                                                (static_cast<float>(fftSize - 1))));
        in[static_cast<size_t>(i)] = samples[static_cast<size_t>(i)] * w;
    }
    OouraFFT fft;
    fft.init(static_cast<size_t>(fftSize));
    fft.fft(in.data(), spectrum.data());
}

// Peak magnitude within +/- searchBins of the nominal bin (leakage tolerance).
static float peakMagnitudeNear(const std::vector<std::complex<float>>& spectrum, float fs, float freq,
                               int searchBins = 2)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const int maxBin = static_cast<int>(spectrum.size()) - 1;
    const int centre = juce::jlimit(1, maxBin, static_cast<int>(std::round(freq * static_cast<float>(fftSize) / fs)));
    float best = 0.0f;
    for (int b = centre - searchBins; b <= centre + searchBins; ++b)
        if (b >= 1 && b <= maxBin)
            best = std::max(best, std::abs(spectrum[static_cast<size_t>(b)]));
    return best;
}

static float spectralCentroidHz(const std::vector<std::complex<float>>& spectrum, float fs, float minHz = 20.0f,
                                float maxHz = 20000.0f)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const float binFreq = fs / static_cast<float>(fftSize);
    double num = 0.0, den = 0.0;
    for (size_t b = 1; b < spectrum.size(); ++b)
    {
        const float f = binFreq * static_cast<float>(b);
        if (f < minHz || f > maxHz)
            continue;
        const double m = std::abs(spectrum[b]);
        num += m * f;
        den += m;
    }
    return den > 0.0 ? static_cast<float>(num / den) : 0.0f;
}

static float bandRmsDb(const std::vector<std::complex<float>>& spectrum, float fs, float loHz, float hiHz)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const float binFreq = fs / static_cast<float>(fftSize);
    double power = 0.0;
    for (size_t b = 1; b < spectrum.size(); ++b)
    {
        const float f = binFreq * static_cast<float>(b);
        if (f >= loHz && f < hiHz)
        {
            const double m = std::abs(spectrum[b]);
            power += m * m;
        }
    }
    // Normalize FFT magnitude to input-referred RMS (Hann coherent gain 0.5).
    const double norm = 2.0 / (0.5 * static_cast<double>(fftSize));
    const double rms = std::sqrt(power) * norm / std::sqrt(2.0);
    return static_cast<float>(20.0 * std::log10(std::max(1.0e-12, rms)));
}

static bool hasNonFinite(const std::vector<float>& v)
{
    for (float s : v)
        if (!std::isfinite(s))
            return true;
    return false;
}

// Named-parameter wrapper around EchoesModel::UpdateParameters so measurement
// intent stays readable and the 60-argument call has one audited call site.
struct ChainConfig
{
    float low = 0.5f, high = 0.5f;
    float intensity = 0.0f;  // feedback ("Intensity" APVTS param, 0-1)
    float delayTime = 0.5f;  // normalized; ~125 ms
    int heads[3] = {1, 0, 0};
    float mix = 1.0f;  // wet only
    bool noiseEnabled = false;
    float noiseAmount = 0.12f;
    float tapeCondition = 0.0f;
    float wowFlutterDepth = 0.0f;
    float wowRate = 1.0f, flutterRate = 1.0f;
    float tapeDrive = 0.5f, tapeSaturation = 0.5f, tapeBias = 0.5f;
    float tapeRecordMakeupGainDb = 0.0f;
    bool outputProtection = false;  // off for linear metrics; on for behavioral runs
    bool feedbackCompensation = true;
    float presetScrapeScale = 1.0f, presetLpfScale = 1.0f;

    void apply(EchoesModel& model)
    {
        int headsCopy[3] = {heads[0], heads[1], heads[2]};
        model.UpdateParameters(
            low, high, intensity, delayTime, headsCopy,
            /*delayenabled*/ 1, mix, /*reverbenabled*/ 0, /*inputlevel*/ 1.0f, /*echoSend*/ 1.0f, noiseEnabled,
            noiseAmount, tapeCondition, wowFlutterDepth, wowRate, flutterRate,
            /*stereoMode*/ 0,
            /*tempoSync*/ false, 0.0f, 0.0f, 0.0f,
            /*binaural*/ false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, tapeDrive, tapeSaturation, tapeBias,
            /*headRatioPreset*/ 0, /*reverbRouting*/ 0,
            /*presetAsperity*/ -1.0f, /*presetDropout*/ -1.0f, presetScrapeScale, presetLpfScale, feedbackCompensation);
        if (auto* tapeDelay = model.getTapeDelay())
        {
            tapeDelay->setTapeCharacter(tapeDrive, tapeSaturation, tapeBias);
            tapeDelay->setTapeRecordMakeupGainDb(tapeRecordMakeupGainDb);
        }
    }
};

static ChainConfig harmonicConfigForFlavor(int flavorIndex, float driveKnob)
{
    const auto flavor = TapeFlavors::resolve(flavorIndex, driveKnob);
    ChainConfig cfg;
    cfg.tapeDrive = flavor.drive;
    cfg.tapeSaturation = flavor.saturation;
    cfg.tapeBias = flavor.bias;
    cfg.tapeRecordMakeupGainDb = TapeFlavors::getTapeRecordMakeupDb(flavorIndex, driveKnob);
    cfg.presetLpfScale = flavor.lpfScale;

    // Keep every production preamp/record/playback/tone stage active while
    // removing stochastic and time-varying flavor sources that would smear
    // energy away from exact harmonic bins.
    cfg.noiseEnabled = false;
    cfg.wowFlutterDepth = 0.0f;
    cfg.tapeCondition = 0.0f;
    return cfg;
}

// Process `seconds` of a generated signal; return the captured left channel
// (only the last `captureSamples` samples). Generator fills a mono block.
template <typename Gen>
static std::vector<float> renderChain(EchoesModel& model, ChainConfig& cfg, float fs, int blockSize, int settleSamples,
                                      int captureSamples, Gen&& gen)
{
    model.Reset(fs, 2, blockSize);
    cfg.apply(model);

    std::vector<std::vector<float>> buf(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    std::vector<float> mono(static_cast<size_t>(blockSize), 0.0f);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(captureSamples));

    const int totalSamples = settleSamples + captureSamples;
    for (int done = 0; done < totalSamples; done += blockSize)
    {
        gen(mono);
        std::copy(mono.begin(), mono.end(), buf[0].begin());
        std::copy(mono.begin(), mono.end(), buf[1].begin());
        model.ProcessBuffer(buf, blockSize);

        for (int s = 0; s < blockSize; ++s)
        {
            const int idx = done + s;
            if (idx >= settleSamples && idx < totalSamples)
                out.push_back(buf[0][static_cast<size_t>(s)]);
        }
    }
    return out;
}

struct SinePhase
{
    float phase = 0.0f;
    void fill(std::vector<float>& block, float fs, float freq, float amp)
    {
        const float twoPi = 2.0f * juce::MathConstants<float>::pi;
        const float inc = twoPi * freq / fs;
        for (float& s : block)
        {
            s = amp * std::sin(phase);
            phase += inc;
            if (phase >= twoPi)
                phase -= twoPi;
        }
    }
};

struct HarmonicProfile
{
    float thdPercent = 0.0f;
    float fundamentalDb = -999.0f;
    float ratio3to2Db = 0.0f;
    std::vector<float> harmonicsDbRel;  // H2..H10 relative to fundamental
    bool finite = true;
};

static HarmonicProfile measureHarmonics(EchoesModel& model, ChainConfig& cfg, float fs, int blockSize, float freq,
                                        float ampLinear)
{
    constexpr int kFftSize = 16384;
    SinePhase osc;
    const auto capture = renderChain(model, cfg, fs, blockSize, static_cast<int>(fs), kFftSize,
                                     [&](std::vector<float>& block) { osc.fill(block, fs, freq, ampLinear); });

    HarmonicProfile p;
    p.finite = !hasNonFinite(capture);
    if (!p.finite)
        return p;

    std::vector<std::complex<float>> spectrum(kFftSize / 2 + 1);
    computeSpectrum(capture, spectrum);

    const float fund = peakMagnitudeNear(spectrum, fs, freq);
    p.fundamentalDb = TestUtilities::linearToDb(std::max(1.0e-12f, fund * 2.0f / (0.5f * kFftSize)));

    double harmPower = 0.0;
    float h2 = 0.0f, h3 = 0.0f;
    for (int h = 2; h <= 10; ++h)
    {
        const float hf = freq * static_cast<float>(h);
        if (hf >= fs * 0.5f)
        {
            p.harmonicsDbRel.push_back(-999.0f);
            continue;
        }
        const float m = peakMagnitudeNear(spectrum, fs, hf);
        if (h == 2)
            h2 = m;
        if (h == 3)
            h3 = m;
        harmPower += static_cast<double>(m) * m;
        p.harmonicsDbRel.push_back(fund > 0.0f ? TestUtilities::linearToDb(std::max(1.0e-12f, m / fund)) : -999.0f);
    }
    p.thdPercent = fund > 0.0f ? 100.0f * static_cast<float>(std::sqrt(harmPower)) / fund : 0.0f;
    p.ratio3to2Db = (h2 > 0.0f && h3 > 0.0f) ? TestUtilities::linearToDb(h3 / h2) : 0.0f;
    return p;
}

}  // namespace baseline

juce::String runTapeChainBaseline()
{
    using namespace baseline;

    const float fs = 48000.0f;
    const int blockSize = 512;
    bool anyNonFinite = false;

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "TapeChainBaseline");
    root->setProperty("sample_rate", static_cast<int>(fs));
    root->setProperty("block_size", blockSize);
    root->setProperty("notes",
                      "Isolated tape chain via EchoesModel: reverb off, binaural off, mix=1 (wet), single head, "
                      "delayTime=0.5 (~125ms). Output protection OFF for THD/harmonics/FR; defaults elsewhere. "
                      "All dB rounded to 0.01 for deterministic output.");

    EchoesModel model;

    // ---- Full-chain WARM harmonic target at the requested levels ----
    {
        juce::Array<juce::var> rows;
        const float freq = alignFrequencyToBin(1000.0f, fs, 16384);
        for (float drive : {0.20f, 0.45f, 0.80f})
        {
            auto cfg = harmonicConfigForFlavor(static_cast<int>(TapeFlavor::Warm), drive);
            auto p = measureHarmonics(model, cfg, fs, blockSize, freq, TestUtilities::dbToLinear(-6.0f));
            anyNonFinite = anyNonFinite || !p.finite;

            auto* row = new juce::DynamicObject();
            row->setProperty("drive", r2(drive));
            row->setProperty("thd_percent", r2(p.thdPercent));
            row->setProperty("h2_db_rel", r2(p.harmonicsDbRel[0]));
            row->setProperty("h3_db_rel", r2(p.harmonicsDbRel[1]));
            row->setProperty("h5_db_rel", r2(p.harmonicsDbRel[3]));
            row->setProperty("ratio_h3_h2_db", r2(p.ratio3to2Db));
            rows.add(juce::var(row));
        }
        root->setProperty("phase4_a1_warm_1k_m6dbfs", rows);
    }

    // ---- Static DRIVE-law harmonic characterization by flavor ----
    {
        juce::Array<juce::var> rows;
        const float freq = alignFrequencyToBin(1000.0f, fs, 16384);
        for (int flavorIndex = 0; flavorIndex < static_cast<int>(TapeFlavor::NumFlavors); ++flavorIndex)
        {
            for (int step = 0; step <= 10; ++step)
            {
                const float drive = static_cast<float>(step) / 10.0f;
                auto cfg = harmonicConfigForFlavor(flavorIndex, drive);
                auto p = measureHarmonics(model, cfg, fs, blockSize, freq, TestUtilities::dbToLinear(-6.0f));
                anyNonFinite = anyNonFinite || !p.finite;

                auto* row = new juce::DynamicObject();
                row->setProperty("flavor", TapeFlavors::get(flavorIndex).name);
                row->setProperty("drive", r2(drive));
                row->setProperty("thd_percent", r2(p.thdPercent));
                row->setProperty("h2_db_rel", r2(p.harmonicsDbRel[0]));
                row->setProperty("h3_db_rel", r2(p.harmonicsDbRel[1]));
                row->setProperty("h5_db_rel", r2(p.harmonicsDbRel[3]));
                rows.add(juce::var(row));
            }
        }
        root->setProperty("phase4_c8_drive_law_1k_m6dbfs", rows);
    }

    // ---- a. THD vs drive (1 kHz bin-aligned, -12 dBFS, no feedback, no wow/noise) ----
    {
        juce::Array<juce::var> rows;
        const float freq = alignFrequencyToBin(1000.0f, fs, 16384);
        for (int step = 0; step <= 10; ++step)
        {
            ChainConfig cfg;
            cfg.tapeDrive = static_cast<float>(step) / 10.0f;
            auto p = measureHarmonics(model, cfg, fs, blockSize, freq, TestUtilities::dbToLinear(-12.0f));
            anyNonFinite = anyNonFinite || !p.finite;

            auto* row = new juce::DynamicObject();
            row->setProperty("drive", r2(cfg.tapeDrive));
            row->setProperty("thd_percent", r2(p.thdPercent));
            row->setProperty("ratio_h3_h2_db", r2(p.ratio3to2Db));
            rows.add(juce::var(row));
        }
        root->setProperty("thd_vs_drive_1k_m12dbfs", rows);
    }

    // ---- b. Harmonic profiles: default at 4 levels + each tape-flavor anchor at -12 dBFS ----
    {
        juce::Array<juce::var> rows;
        const float freq = alignFrequencyToBin(1000.0f, fs, 16384);

        auto addProfile = [&](const juce::String& name, ChainConfig& cfg, float levelDb)
        {
            auto p = measureHarmonics(model, cfg, fs, blockSize, freq, TestUtilities::dbToLinear(levelDb));
            anyNonFinite = anyNonFinite || !p.finite;
            auto* row = new juce::DynamicObject();
            row->setProperty("name", name);
            row->setProperty("input_dbfs", r2(levelDb));
            row->setProperty("thd_percent", r2(p.thdPercent));
            row->setProperty("ratio_h3_h2_db", r2(p.ratio3to2Db));
            juce::Array<juce::var> harm;
            for (float h : p.harmonicsDbRel)
                harm.add(r2(h));
            row->setProperty("h2_to_h10_db_rel", harm);
            rows.add(juce::var(row));
        };

        for (float levelDb : {-20.0f, -12.0f, -6.0f, 0.0f})
        {
            ChainConfig cfg;
            addProfile("default", cfg, levelDb);
        }

        for (int flavorIndex = 0; flavorIndex < static_cast<int>(TapeFlavor::NumFlavors); ++flavorIndex)
        {
            const auto resolved = TapeFlavors::resolve(flavorIndex, TapeFlavors::kDriveAnchor);
            ChainConfig cfg;
            cfg.tapeDrive = resolved.drive;
            cfg.tapeSaturation = resolved.saturation;
            cfg.tapeBias = resolved.bias;
            cfg.tapeRecordMakeupGainDb = TapeFlavors::getTapeRecordMakeupDb(flavorIndex, TapeFlavors::kDriveAnchor);
            cfg.tapeCondition = resolved.condition;
            cfg.presetLpfScale = resolved.lpfScale;
            addProfile(juce::String(TapeFlavors::get(flavorIndex).name), cfg, -12.0f);
        }
        root->setProperty("harmonic_profiles", rows);
    }

    // ---- c. Wow/flutter sidebands on 3150 Hz (2^20-point capture, ~21.8 s) ----
    {
        juce::Array<juce::var> rows;
        constexpr int kFftSize = 1 << 20;
        const float freq = alignFrequencyToBin(3150.0f, fs, kFftSize);

        // (depth, condition) pairs; the condition≥0.25 rows exist to measure the
        // scrape-flutter band (the scrape gate opens above condition 0.15).
        const std::pair<float, float> variants[] = {
            {0.25f, 0.08f}, {0.5f, 0.08f}, {1.0f, 0.08f}, {0.5f, 0.25f}, {0.5f, 0.5f},
        };
        for (const auto& [depth, condition] : variants)
        {
            ChainConfig cfg;
            cfg.wowFlutterDepth = depth;
            cfg.tapeCondition = condition;
            SinePhase osc;
            const auto capture = renderChain(model, cfg, fs, blockSize, static_cast<int>(2.0f * fs), kFftSize,
                                             [&](std::vector<float>& block)
                                             { osc.fill(block, fs, freq, TestUtilities::dbToLinear(-12.0f)); });
            anyNonFinite = anyNonFinite || hasNonFinite(capture);

            std::vector<std::complex<float>> spectrum(kFftSize / 2 + 1);
            computeSpectrum(capture, spectrum);

            const float binFreq = fs / static_cast<float>(kFftSize);
            const int carrierBin = static_cast<int>(std::round(freq / binFreq));
            const float carrier = peakMagnitudeNear(spectrum, fs, freq, 2);

            auto sidebandPeak = [&](float loOff, float hiOff, bool upper) -> std::pair<float, float>
            {
                float best = 0.0f, bestOff = 0.0f;
                const int lo = static_cast<int>(std::round(loOff / binFreq));
                const int hi = static_cast<int>(std::round(hiOff / binFreq));
                for (int off = lo; off <= hi; ++off)
                {
                    const int bin = upper ? carrierBin + off : carrierBin - off;
                    if (bin < 1 || bin >= static_cast<int>(spectrum.size()))
                        continue;
                    const float m = std::abs(spectrum[static_cast<size_t>(bin)]);
                    if (m > best)
                    {
                        best = m;
                        bestOff = static_cast<float>(off) * binFreq;
                    }
                }
                const float rel =
                    carrier > 0.0f ? TestUtilities::linearToDb(std::max(1.0e-12f, best / carrier)) : -999.0f;
                return {rel, bestOff};
            };

            double sbPower = 0.0;
            const int loBin = static_cast<int>(std::round(0.4f / binFreq));
            const int hiBin = static_cast<int>(std::round(30.0f / binFreq));
            for (int off = loBin; off <= hiBin; ++off)
            {
                for (int sign : {-1, 1})
                {
                    const int bin = carrierBin + sign * off;
                    if (bin >= 1 && bin < static_cast<int>(spectrum.size()))
                    {
                        const double m = std::abs(spectrum[static_cast<size_t>(bin)]);
                        sbPower += m * m;
                    }
                }
            }
            const float sbTotalRelDb =
                carrier > 0.0f
                    ? TestUtilities::linearToDb(std::max(1.0e-12f, static_cast<float>(std::sqrt(sbPower)) / carrier))
                    : -999.0f;

            // Scrape band: sideband power 100-2000 Hz away from the carrier
            // (scrape oscillators run at 350-2800 Hz; their FM sidebands land here).
            double scrapePower = 0.0;
            const int sLo = static_cast<int>(std::round(100.0f / binFreq));
            const int sHi = static_cast<int>(std::round(2000.0f / binFreq));
            for (int off = sLo; off <= sHi; ++off)
            {
                for (int sign : {-1, 1})
                {
                    const int bin = carrierBin + sign * off;
                    if (bin >= 1 && bin < static_cast<int>(spectrum.size()))
                    {
                        const double m = std::abs(spectrum[static_cast<size_t>(bin)]);
                        scrapePower += m * m;
                    }
                }
            }
            const float scrapeRelDb =
                carrier > 0.0f ? TestUtilities::linearToDb(
                                     std::max(1.0e-12f, static_cast<float>(std::sqrt(scrapePower)) / carrier))
                               : -999.0f;

            auto* row = new juce::DynamicObject();
            row->setProperty("depth", r2(depth));
            row->setProperty("condition", r2(condition));
            const auto up = sidebandPeak(0.4f, 30.0f, true);
            const auto dn = sidebandPeak(0.4f, 30.0f, false);
            row->setProperty("carrier_hz", r2(freq));
            row->setProperty("scrape_band_power_db_rel_100_2000hz", r2(scrapeRelDb));
            row->setProperty("peak_upper_sideband_db_rel", r2(up.first));
            row->setProperty("peak_upper_sideband_offset_hz", r2(up.second));
            row->setProperty("peak_lower_sideband_db_rel", r2(dn.first));
            row->setProperty("peak_lower_sideband_offset_hz", r2(dn.second));
            row->setProperty("total_sideband_power_db_rel_pm30hz", r2(sbTotalRelDb));
            rows.add(juce::var(row));
        }
        root->setProperty("wow_flutter_sidebands_3150", rows);
    }

    // ---- d. Hiss floor (silence input, noise enabled) ----
    {
        juce::Array<juce::var> rows;
        for (float amount : {0.12f, 0.5f})
        {
            for (float condition : {0.08f, 0.5f, 1.0f})
            {
                ChainConfig cfg;
                cfg.noiseEnabled = true;
                cfg.noiseAmount = amount;
                cfg.tapeCondition = condition;
                cfg.intensity = 0.4f;              // APVTS default feedback, representative
                constexpr int kFftSize = 1 << 17;  // 2.73 s analysis window
                const auto capture =
                    renderChain(model, cfg, fs, blockSize, static_cast<int>(fs), kFftSize,
                                [](std::vector<float>& block) { std::fill(block.begin(), block.end(), 0.0f); });
                anyNonFinite = anyNonFinite || hasNonFinite(capture);

                float rms = TestUtilities::rmsAbs(capture.data(), static_cast<int>(capture.size()));
                std::vector<std::complex<float>> spectrum(kFftSize / 2 + 1);
                computeSpectrum(capture, spectrum);

                auto* row = new juce::DynamicObject();
                row->setProperty("noise_amount", r2(amount));
                row->setProperty("condition", r2(condition));
                row->setProperty("rms_dbfs", r2(TestUtilities::linearToDb(std::max(1.0e-12f, rms))));
                auto* bands = new juce::DynamicObject();
                for (float centre : {1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f})
                    bands->setProperty("band_" + juce::String(static_cast<int>(centre)),
                                       r2(bandRmsDb(spectrum, fs, centre / 1.41421356f, centre * 1.41421356f)));
                row->setProperty("octave_bands_dbfs", juce::var(bands));
                rows.add(juce::var(row));
            }
        }
        root->setProperty("hiss_floor", rows);
    }

    // ---- e. Frequency response per tape-flavor anchor (single pass, -12 dBFS) ----
    {
        juce::Array<juce::var> rows;
        constexpr int kFftSize = 16384;
        std::vector<float> freqs;
        for (int i = 0; i < 24; ++i)
        {
            const float f = 50.0f * std::pow(16000.0f / 50.0f, static_cast<float>(i) / 23.0f);
            freqs.push_back(alignFrequencyToBin(f, fs, kFftSize));
        }

        auto measureFr = [&](const juce::String& name, ChainConfig cfgTemplate)
        {
            juce::Array<juce::var> points;
            float magAt1k = 0.0f;
            std::vector<float> mags;
            for (float f : freqs)
            {
                ChainConfig cfg = cfgTemplate;
                SinePhase osc;
                const auto capture = renderChain(model, cfg, fs, blockSize, static_cast<int>(0.75f * fs), kFftSize,
                                                 [&](std::vector<float>& block)
                                                 { osc.fill(block, fs, f, TestUtilities::dbToLinear(-12.0f)); });
                anyNonFinite = anyNonFinite || hasNonFinite(capture);
                std::vector<std::complex<float>> spectrum(kFftSize / 2 + 1);
                computeSpectrum(capture, spectrum);
                const float m = peakMagnitudeNear(spectrum, fs, f);
                mags.push_back(m);
                if (std::abs(f - 1000.0f) < 30.0f)
                    magAt1k = m;
            }
            for (size_t i = 0; i < freqs.size(); ++i)
            {
                auto* pt = new juce::DynamicObject();
                pt->setProperty("hz", r2(freqs[i]));
                pt->setProperty(
                    "mag_db_re_1k",
                    r2(magAt1k > 0.0f ? TestUtilities::linearToDb(std::max(1.0e-12f, mags[i] / magAt1k)) : -999.0f));
                points.add(juce::var(pt));
            }
            auto* row = new juce::DynamicObject();
            row->setProperty("name", name);
            row->setProperty("points", points);
            rows.add(juce::var(row));
        };

        measureFr("default", ChainConfig{});
        for (int flavorIndex = 0; flavorIndex < static_cast<int>(TapeFlavor::NumFlavors); ++flavorIndex)
        {
            const auto resolved = TapeFlavors::resolve(flavorIndex, TapeFlavors::kDriveAnchor);
            ChainConfig cfg;
            cfg.tapeDrive = resolved.drive;
            cfg.tapeSaturation = resolved.saturation;
            cfg.tapeBias = resolved.bias;
            cfg.tapeRecordMakeupGainDb = TapeFlavors::getTapeRecordMakeupDb(flavorIndex, TapeFlavors::kDriveAnchor);
            cfg.tapeCondition = resolved.condition;
            cfg.presetLpfScale = resolved.lpfScale;
            measureFr(juce::String(TapeFlavors::get(flavorIndex).name), cfg);
        }
        root->setProperty("frequency_response", rows);
    }

    // ---- e2. Loop-noise intensity curve (TapeNoiseProcessor direct) ----
    {
        juce::Array<juce::var> points;
        TapeNoiseProcessor noise;
        for (int step = 0; step <= 100; ++step)
        {
            const float intensity = static_cast<float>(step) / 100.0f;
            noise.setDeterministicSeed(0xC0FFEEu);
            noise.reset(fs);
            noise.updateParameters(true, 0.5f, 0.5f, intensity);
            // Settle gain smoothing (tau ~45 ms), then measure loop-noise RMS.
            for (int i = 0; i < 12000; ++i)
                noise.processSample(0.0f);
            double sumSq = 0.0;
            constexpr int kMeasure = 8192;
            for (int i = 0; i < kMeasure; ++i)
            {
                noise.processSample(0.0f);
                const double v = noise.getLoopNoise();
                sumSq += v * v;
            }
            const float rms = static_cast<float>(std::sqrt(sumSq / kMeasure));
            auto* pt = new juce::DynamicObject();
            pt->setProperty("intensity", r2(intensity));
            pt->setProperty("loop_noise_rms_dbfs", r2(TestUtilities::linearToDb(std::max(1.0e-12f, rms))));
            points.add(juce::var(pt));
        }
        root->setProperty("loop_noise_intensity_curve", points);
    }

    // ---- f. Self-oscillation at max intensity (real behavior: protection+comp default) ----
    {
        juce::Array<juce::var> rows;
        for (float seedDb : {-20.0f, 0.0f})
            for (bool compEnabled : {true, false})
            {
                ChainConfig cfg;
                cfg.intensity = 1.0f;
                cfg.noiseEnabled = true;  // loop noise seeds oscillation
                cfg.tapeCondition = 0.08f;
                cfg.outputProtection = true;
                cfg.feedbackCompensation = compEnabled;

                model.Reset(fs, 2, blockSize);
                cfg.apply(model);

                std::vector<std::vector<float>> buf(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
                const int totalSeconds = 30;
                juce::Array<juce::var> perSecondPeakDb;
                std::vector<float> tail;  // last ~5.46 s for spectrum
                constexpr int kTailSize = 1 << 18;
                tail.reserve(kTailSize);
                float secondPeak = 0.0f;
                int samplesIntoSecond = 0;
                bool sawNonFinite = false;

                // Seed the loop with a 1 s, 200 Hz burst: silent loops are
                // intentionally gated (loop-noise gate opens above ~-55 dBFS), so
                // ungated self-oscillation behavior only shows after program material.
                // Two seed levels: -20 dBFS (quiet program) and 0 dBFS (hot program -
                // required to engage the feedback compensation limiter at all).
                const int seedSamples = static_cast<int>(fs);
                SinePhase seedOsc;
                const float seedAmp = TestUtilities::dbToLinear(seedDb);

                for (int done = 0; done < totalSeconds * static_cast<int>(fs); done += blockSize)
                {
                    if (done < seedSamples)
                    {
                        std::vector<float> seed(static_cast<size_t>(blockSize), 0.0f);
                        seedOsc.fill(seed, fs, 200.0f, seedAmp);
                        std::copy(seed.begin(), seed.end(), buf[0].begin());
                        std::copy(seed.begin(), seed.end(), buf[1].begin());
                    }
                    else
                    {
                        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
                        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
                    }
                    model.ProcessBuffer(buf, blockSize);
                    for (int s = 0; s < blockSize; ++s)
                    {
                        const float v = buf[0][static_cast<size_t>(s)];
                        if (!std::isfinite(v))
                        {
                            sawNonFinite = true;
                            continue;
                        }
                        secondPeak = std::max(secondPeak, std::abs(v));
                        if (static_cast<int>(tail.size()) >= kTailSize)
                            tail.erase(tail.begin(), tail.begin() + blockSize);
                        tail.push_back(v);
                        if (++samplesIntoSecond >= static_cast<int>(fs))
                        {
                            perSecondPeakDb.add(r2(TestUtilities::linearToDb(std::max(1.0e-12f, secondPeak))));
                            secondPeak = 0.0f;
                            samplesIntoSecond = 0;
                        }
                    }
                }
                anyNonFinite = anyNonFinite || sawNonFinite;

                tail.resize(kTailSize, 0.0f);
                std::vector<std::complex<float>> spectrum(kTailSize / 2 + 1);
                computeSpectrum(tail, spectrum);
                float domMag = 0.0f, domHz = 0.0f;
                const float binFreq = fs / static_cast<float>(kTailSize);
                for (size_t b = static_cast<size_t>(20.0f / binFreq); b < spectrum.size(); ++b)
                {
                    const float m = std::abs(spectrum[b]);
                    if (m > domMag)
                    {
                        domMag = m;
                        domHz = binFreq * static_cast<float>(b);
                    }
                }

                auto* row = new juce::DynamicObject();
                row->setProperty("feedback_compensation", compEnabled);
                row->setProperty("intensity", 1.0);
                row->setProperty("seed_dbfs", r2(seedDb));
                row->setProperty("per_second_peak_dbfs", perSecondPeakDb);
                row->setProperty("dominant_hz_final5s", r2(domHz));
                row->setProperty("spectral_centroid_hz_final5s", r2(spectralCentroidHz(spectrum, fs)));
                row->setProperty("non_finite_samples", sawNonFinite);
                rows.add(juce::var(row));
            }
        root->setProperty("self_oscillation", rows);
    }

    // ---- g. Regeneration darkening: impulse, 70% feedback, first 8 repeats ----
    {
        ChainConfig cfg;
        cfg.intensity = 0.7f;
        cfg.outputProtection = true;

        model.Reset(fs, 2, blockSize);
        cfg.apply(model);

        std::vector<std::vector<float>> buf(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        const int captureSamples = 10 * static_cast<int>(fs);
        std::vector<float> capture;
        capture.reserve(static_cast<size_t>(captureSamples));
        bool impulseSent = false;

        for (int done = 0; done < captureSamples; done += blockSize)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            if (!impulseSent)
            {
                buf[0][0] = 0.9f;
                buf[1][0] = 0.9f;
                impulseSent = true;
            }
            model.ProcessBuffer(buf, blockSize);
            for (int s = 0; s < blockSize && static_cast<int>(capture.size()) < captureSamples; ++s)
                capture.push_back(buf[0][static_cast<size_t>(s)]);
        }
        anyNonFinite = anyNonFinite || hasNonFinite(capture);

        // First repeat: first |x| > 0.02 after 10 ms.
        int firstPeakIdx = -1;
        float firstPeakVal = 0.0f;
        const int searchStart = static_cast<int>(0.010f * fs);
        for (int i = searchStart; i < static_cast<int>(capture.size()); ++i)
        {
            const float a = std::abs(capture[static_cast<size_t>(i)]);
            if (a > 0.02f)
            {
                // refine to local max within the next 5 ms
                int end = std::min<int>(i + static_cast<int>(0.005f * fs), static_cast<int>(capture.size()));
                firstPeakIdx = i;
                for (int k = i; k < end; ++k)
                    if (std::abs(capture[static_cast<size_t>(k)]) >
                        std::abs(capture[static_cast<size_t>(firstPeakIdx)]))
                        firstPeakIdx = k;
                firstPeakVal = std::abs(capture[static_cast<size_t>(firstPeakIdx)]);
                break;
            }
        }

        juce::Array<juce::var> repeats;
        double delaySamples = firstPeakIdx > 0 ? static_cast<double>(firstPeakIdx) : 0.0;
        if (firstPeakIdx > 0)
        {
            for (int k = 1; k <= 8; ++k)
            {
                const int centre = static_cast<int>(std::round(delaySamples * k));
                const int half = static_cast<int>(0.030f * fs);
                const int lo = std::max(0, centre - half);
                const int hi = std::min<int>(static_cast<int>(capture.size()), centre + half);
                if (hi - lo < 256)
                    break;

                int peakIdx = lo;
                for (int i = lo; i < hi; ++i)
                    if (std::abs(capture[static_cast<size_t>(i)]) > std::abs(capture[static_cast<size_t>(peakIdx)]))
                        peakIdx = i;

                constexpr int kWin = 8192;
                const int wLo = std::max(0, peakIdx - kWin / 4);
                std::vector<float> window(kWin, 0.0f);
                for (int i = 0; i < kWin && wLo + i < static_cast<int>(capture.size()); ++i)
                    window[static_cast<size_t>(i)] = capture[static_cast<size_t>(wLo + i)];

                const float rms = TestUtilities::rmsAbs(window.data(), kWin);
                std::vector<std::complex<float>> spectrum(kWin / 2 + 1);
                computeSpectrum(window, spectrum);

                auto* row = new juce::DynamicObject();
                row->setProperty("repeat", k);
                row->setProperty("peak_time_ms", r2(1000.0 * peakIdx / fs));
                row->setProperty("rms_dbfs", r2(TestUtilities::linearToDb(std::max(1.0e-12f, rms))));
                row->setProperty("spectral_centroid_hz", r2(spectralCentroidHz(spectrum, fs, 40.0f, 16000.0f)));
                repeats.add(juce::var(row));
            }
        }

        auto* regen = new juce::DynamicObject();
        regen->setProperty("first_repeat_ms", r2(firstPeakIdx > 0 ? 1000.0 * firstPeakIdx / fs : -1.0));
        regen->setProperty("first_repeat_peak_dbfs", r2(TestUtilities::linearToDb(std::max(1.0e-12f, firstPeakVal))));
        regen->setProperty("repeats", repeats);
        root->setProperty("feedback_regeneration", juce::var(regen));
    }

    root->setProperty("any_non_finite", anyNonFinite);

    const juce::var tree(root);
    const juce::String json = juce::JSON::toString(tree, false);
    juce::File jsonFile = artifactDirectory().getChildFile("tape_chain_baseline.json");
    jsonFile.deleteFile();
    const bool wroteJson = jsonFile.replaceWithText(json);

    juce::String result;
    result << "TapeChainBaseline\n[INFO] JSON: " << jsonFile.getFullPathName() << "\n"
           << ((anyNonFinite || !wroteJson) ? "[FAIL]" : "[PASS]");
    if (anyNonFinite)
        result << " non-finite samples detected";
    if (!wroteJson)
        result << " JSON write failed";
    return result;
}

}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
