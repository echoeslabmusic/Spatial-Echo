// TapePresetBaselineRenders: Render 3 deterministic signals through every factory preset (and the INIT state) via the
//   real plugin processBlock path. WAVs are audition artifacts (untracked); the metrics JSON is the machine-comparable
//   record used for re-authoring presets.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "TapeTests.h"
#include "../../PluginProcessor.h"
#include "../../DSP/OouraFFT.h"
#include "../../Presets/FactoryPresetLibrary.h"
#include "../Common/TestUtilities.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace SpatialEchoTests::TapeEmulation
{

namespace renders
{

static juce::File artifactDirectory()
{
    auto dir = SpatialEchoTests::artifactDirectory().getChildFile("tape-baseline").getChildFile("preset-renders");
    dir.createDirectory();
    return dir;
}

static double r2(double v)
{
    if (!std::isfinite(v))
        return -999.0;
    return std::round(v * 100.0) / 100.0;
}

// Deterministic LCG so renders are byte-identical across runs.
struct Rng
{
    uint32_t state;
    explicit Rng(uint32_t seed)
        : state(seed)
    {
    }
    float bipolar()
    {
        state = state * 1664525u + 1013904223u;
        return (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
    }
};

// 6 s at 48 kHz. Three signal types, all peak-normalized by construction.
enum class Signal
{
    Drum,
    Sustained,
    Mix
};

static const char* signalSlug(Signal s)
{
    switch (s)
    {
        case Signal::Drum:
            return "drum";
        case Signal::Sustained:
            return "sustained";
        case Signal::Mix:
            return "mix";
    }
    return "mix";
}

static void generateSignal(Signal type, std::vector<float>& out, float fs)
{
    const int n = static_cast<int>(out.size());
    std::fill(out.begin(), out.end(), 0.0f);

    auto addDrum = [&](float gain)
    {
        Rng rng(0xBA5E1u);
        const int burstInterval = static_cast<int>(0.5f * fs);  // 120 BPM eighths
        float lp = 0.0f;
        const float lpCoeff = std::exp(-2.0f * juce::MathConstants<float>::pi * 3500.0f / fs);
        for (int b = 0; b * burstInterval < n; ++b)
        {
            const int start = b * burstInterval;
            const int len = static_cast<int>(0.12f * fs);
            for (int i = 0; i < len && start + i < n; ++i)
            {
                const float env = std::exp(-static_cast<float>(i) / (0.045f * fs));
                const float noise = rng.bipolar();
                lp = lpCoeff * lp + (1.0f - lpCoeff) * noise;
                out[static_cast<size_t>(start + i)] += gain * env * lp * 3.0f;
            }
        }
    };

    auto addSustained = [&](float gain)
    {
        // Harmonic-rich tone (saw-like, 12 partials) at 220.13 Hz (bin-aligned @16384).
        const float f0 = 220.13f;
        for (int h = 1; h <= 12; ++h)
        {
            const float amp = gain * 0.35f / static_cast<float>(h);
            const float inc = 2.0f * juce::MathConstants<float>::pi * f0 * static_cast<float>(h) / fs;
            float phase = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                out[static_cast<size_t>(i)] += amp * std::sin(phase);
                phase += inc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
            }
        }
    };

    switch (type)
    {
        case Signal::Drum:
            addDrum(0.9f);
            break;
        case Signal::Sustained:
            addSustained(0.7f);
            break;
        case Signal::Mix:
            addDrum(0.55f);
            addSustained(0.4f);
            break;
    }

    float peak = TestUtilities::peakAbs(out.data(), n);
    if (peak > 0.0f)
    {
        const float target = TestUtilities::dbToLinear(-6.0f);
        const float g = target / peak;
        for (float& s : out)
            s *= g;
    }
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

static float spectralCentroidHz(const std::vector<std::complex<float>>& spectrum, float fs)
{
    const int fftSize = static_cast<int>(spectrum.size() * 2 - 2);
    const float binFreq = fs / static_cast<float>(fftSize);
    double num = 0.0, den = 0.0;
    for (size_t b = 1; b < spectrum.size(); ++b)
    {
        const float f = binFreq * static_cast<float>(b);
        if (f < 20.0f || f > 20000.0f)
            continue;
        const double m = std::abs(spectrum[b]);
        num += m * f;
        den += m;
    }
    return den > 0.0 ? static_cast<float>(num / den) : 0.0f;
}

static float bandEnergyDb(const std::vector<std::complex<float>>& spectrum, float fs, float loHz, float hiHz)
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
    return static_cast<float>(10.0 * std::log10(std::max(1.0e-24, power)));
}

static bool writeWav(const juce::File& file, const std::vector<float>& left, const std::vector<float>& right, float fs)
{
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                           .withSampleRate(static_cast<double>(fs))
                                                           .withNumChannels(2)
                                                           .withBitsPerSample(24));
    if (writer == nullptr)
        return false;

    juce::AudioBuffer<float> buf(2, static_cast<int>(left.size()));
    for (int i = 0; i < static_cast<int>(left.size()); ++i)
    {
        buf.setSample(0, i, left[static_cast<size_t>(i)]);
        buf.setSample(1, i, right[static_cast<size_t>(i)]);
    }
    writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    return true;
}

struct RenderMetrics
{
    float rmsDb = -999.0f;
    float peakDb = -999.0f;
    float crestDb = 0.0f;
    float centroidHz = 0.0f;
    float bandLowDb = -999.0f;   // 40-250 Hz
    float bandMidDb = -999.0f;   // 250-2000 Hz
    float bandHighDb = -999.0f;  // 2k-8k Hz
    float bandAirDb = -999.0f;   // 8k-16k Hz
    bool finite = true;
};

static RenderMetrics analyse(const std::vector<float>& mono, float fs)
{
    RenderMetrics m;
    for (float s : mono)
        if (!std::isfinite(s))
        {
            m.finite = false;
            return m;
        }

    const float rms = TestUtilities::rmsAbs(mono.data(), static_cast<int>(mono.size()));
    const float peak = TestUtilities::peakAbs(mono.data(), static_cast<int>(mono.size()));
    m.rmsDb = TestUtilities::linearToDb(std::max(1.0e-12f, rms));
    m.peakDb = TestUtilities::linearToDb(std::max(1.0e-12f, peak));
    m.crestDb = m.peakDb - m.rmsDb;

    constexpr int kFftSize = 1 << 17;
    std::vector<float> window(mono.begin(),
                              mono.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(mono.size(), kFftSize)));
    window.resize(kFftSize, 0.0f);
    std::vector<std::complex<float>> spectrum(kFftSize / 2 + 1);
    computeSpectrum(window, spectrum);

    m.centroidHz = spectralCentroidHz(spectrum, fs);
    m.bandLowDb = bandEnergyDb(spectrum, fs, 40.0f, 250.0f);
    m.bandMidDb = bandEnergyDb(spectrum, fs, 250.0f, 2000.0f);
    m.bandHighDb = bandEnergyDb(spectrum, fs, 2000.0f, 8000.0f);
    m.bandAirDb = bandEnergyDb(spectrum, fs, 8000.0f, 16000.0f);
    return m;
}

}  // namespace renders

// ============================================================
//  SRSpotBaselineRenders
//  Purpose:         Render a small factory-preset matrix at 44.1 and 96 kHz.
//  Realtime-safety: N/A (test-only code)
//  Reference:       TapePresetBaselineRenders
// ============================================================

juce::String runTapePresetBaselineRenders(SpatialEchoAudioProcessor& processor)
{
    using namespace renders;

    const double fs = 48000.0;
    const int blockSize = 512;
    const int renderSamples = 6 * 48000;
    const int preRollSamples = 48000;  // settle after preset load, not captured
    bool anyFailure = false;

    const juce::File outDir = artifactDirectory();
    // Cross-platform render comparison needs the metrics only.
    const bool metricsOnly =
        juce::SystemStats::getEnvironmentVariable("SPATIAL_ECHO_RENDER_METRICS_ONLY", {}).isNotEmpty();
    auto* root = new juce::DynamicObject();
    root->setProperty("test", "TapePresetBaselineRenders");
    root->setProperty("sample_rate", 48000);
    root->setProperty("render_seconds", 6);
    root->setProperty("notes", "Full plugin processBlock path, preset applied via loadFactoryPreset(). "
                               "Signals: drum (deterministic filtered noise bursts, 120 BPM), sustained "
                               "(220.13 Hz, 12 partials), mix (both). Peak-normalized to -6 dBFS at input. "
                               "Metrics from left channel. WAVs are untracked audition artifacts.");

    juce::Array<juce::var> presetRows;

    // Pre-generate the three input signals once (deterministic).
    std::vector<std::vector<float>> signals(3, std::vector<float>(static_cast<size_t>(renderSamples), 0.0f));
    generateSignal(Signal::Drum, signals[0], static_cast<float>(fs));
    generateSignal(Signal::Sustained, signals[1], static_cast<float>(fs));
    generateSignal(Signal::Mix, signals[2], static_cast<float>(fs));

    auto renderConfig = [&](const juce::String& slug, const juce::String& displayName, int presetIndex)
    {
        auto* row = new juce::DynamicObject();
        row->setProperty("name", displayName);
        row->setProperty("index", presetIndex);

        for (int sig = 0; sig < 3; ++sig)
        {
            const Signal type = static_cast<Signal>(sig);

            // Fresh DSP state per render, then apply the preset.
            processor.prepareToPlay(fs, blockSize);
            if (presetIndex >= 0)
                processor.loadFactoryPreset(displayName);

            juce::AudioBuffer<float> block(2, blockSize);
            juce::MidiBuffer midi;
            std::vector<float> outL, outR;
            outL.reserve(static_cast<size_t>(renderSamples));
            outR.reserve(static_cast<size_t>(renderSamples));

            const auto& input = signals[static_cast<size_t>(sig)];
            for (int done = 0; done < preRollSamples + renderSamples; done += blockSize)
            {
                block.clear();
                for (int s = 0; s < blockSize; ++s)
                {
                    const int idx = done + s - preRollSamples;
                    // Pre-roll feeds the first second of the signal on loop to settle
                    // smoothing without leaking un-captured transients into the capture.
                    const float v = idx >= 0 && idx < renderSamples
                                        ? input[static_cast<size_t>(idx)]
                                        : input[static_cast<size_t>((done + s) % 48000)] * 0.25f;
                    block.setSample(0, s, v);
                    block.setSample(1, s, v);
                }
                processor.processBlock(block, midi);
                for (int s = 0; s < blockSize; ++s)
                {
                    const int idx = done + s - preRollSamples;
                    if (idx >= 0 && idx < renderSamples)
                    {
                        outL.push_back(block.getSample(0, s));
                        outR.push_back(block.getSample(1, s));
                    }
                }
            }

            const auto metrics = analyse(outL, static_cast<float>(fs));
            const auto metricsR = analyse(outR, static_cast<float>(fs));
            anyFailure = anyFailure || !metrics.finite || !metricsR.finite;

            const juce::String wavName = juce::String(presetIndex >= 0 ? presetIndex : 99).paddedLeft('0', 2) + "_" +
                                         slug + "_" + signalSlug(type) + ".wav";
            if (!metricsOnly && !writeWav(outDir.getChildFile(wavName), outL, outR, static_cast<float>(fs)))
                anyFailure = true;

            auto* sm = new juce::DynamicObject();
            sm->setProperty("rms_dbfs", r2(metrics.rmsDb));
            sm->setProperty("peak_dbfs", r2(metrics.peakDb));
            sm->setProperty("crest_db", r2(metrics.crestDb));
            sm->setProperty("spectral_centroid_hz", r2(metrics.centroidHz));
            sm->setProperty("band_low_db", r2(metrics.bandLowDb));
            sm->setProperty("band_mid_db", r2(metrics.bandMidDb));
            sm->setProperty("band_high_db", r2(metrics.bandHighDb));
            sm->setProperty("band_air_db", r2(metrics.bandAirDb));
            sm->setProperty("rms_r_dbfs", r2(metricsR.rmsDb));
            sm->setProperty("spectral_centroid_r_hz", r2(metricsR.centroidHz));
            sm->setProperty("finite", metrics.finite && metricsR.finite);
            row->setProperty(signalSlug(type), juce::var(sm));
        }
        presetRows.add(juce::var(row));
    };

    // INIT/default state first (no preset applied).
    renderConfig("default-state", "default_state", -1);

    for (size_t i = 0; i < FactoryPresetLibrary::kFactoryPresets.size(); ++i)
    {
        const auto& preset = FactoryPresetLibrary::kFactoryPresets[i];
        juce::String slug = juce::String(preset.name)
                                .toLowerCase()
                                .replaceCharacters(" /()'&", "------")
                                .retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789-");
        renderConfig(slug, juce::String(preset.name), static_cast<int>(i));
    }

    root->setProperty("presets", presetRows);
    root->setProperty("any_failure", anyFailure);

    juce::File jsonFile = outDir.getParentDirectory().getChildFile("preset_metrics.json");
    jsonFile.deleteFile();
    if (!jsonFile.replaceWithText(juce::JSON::toString(juce::var(root), false)))
        anyFailure = true;

    juce::String result;
    result << "TapePresetBaselineRenders\n[INFO] metrics: " << jsonFile.getFullPathName()
           << "\n[INFO] renders: " << outDir.getFullPathName() << "\n"
           << (anyFailure ? "[FAIL] non-finite output or WAV/JSON write failure" : "[PASS]");
    return result;
}

}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
