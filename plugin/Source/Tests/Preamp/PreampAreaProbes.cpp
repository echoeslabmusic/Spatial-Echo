// PreampAreaProbes: Preamp-stage-ISOLATED magnitude response and -3 dB corner measurement, kept in the repo so the
//   number is reproducible on demand.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "PreampTests.h"
#include "../Common/TestUtilities.h"
#include "../../DSP/PreampProcessor.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace SpatialEchoTests::Preamp
{
namespace
{
constexpr float kSampleRate = 48000.0f;
constexpr float kAmplitude = 0.05f;  // linear region: response, not distortion
constexpr float kReferenceHz = 500.0f;

double measureResponseRms(float freq)
{
    const int samples = static_cast<int>(std::ceil(kSampleRate * 0.15f));
    const int warmupSamples = static_cast<int>(std::ceil(kSampleRate * 0.05f));
    PreampProcessor preamp;
    preamp.reset(kSampleRate, 1, samples);
    preamp.updateParameters(1.0f);

    std::vector<std::vector<float>> warmup(1, std::vector<float>(static_cast<size_t>(warmupSamples), 0.0f));
    preamp.processBuffer(warmup, warmupSamples);

    std::vector<std::vector<float>> buffer(1, std::vector<float>(static_cast<size_t>(samples), 0.0f));
    for (int i = 0; i < samples; ++i)
        buffer[0][static_cast<size_t>(i)] =
            kAmplitude * std::sin(2.0f * juce::MathConstants<float>::pi * freq * static_cast<float>(i) / kSampleRate);
    preamp.processBuffer(buffer, samples);

    // Skip the first third to exclude filter transients.
    const int start = samples / 3;
    double energy = 0.0;
    for (int i = start; i < samples; ++i)
        energy += static_cast<double>(buffer[0][static_cast<size_t>(i)]) * buffer[0][static_cast<size_t>(i)];
    return std::sqrt(energy / std::max(1, samples - start));
}
}  // namespace

juce::String runPreampInputLoadingCornerProbe(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    const std::vector<float> freqs{250.0f,  500.0f,  1000.0f, 1500.0f,  2000.0f,  3000.0f, 4000.0f,
                                   5000.0f, 6000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f};

    const double refRms = measureResponseRms(kReferenceHz);
    bool finite = refRms > 0.0 && std::isfinite(refRms);

    juce::Array<juce::var> rows;
    std::vector<double> relDb(freqs.size(), 0.0);
    for (size_t i = 0; i < freqs.size(); ++i)
    {
        const double rms = measureResponseRms(freqs[i]);
        finite = finite && std::isfinite(rms) && rms > 0.0;
        relDb[i] = 20.0 * std::log10(std::max(1.0e-12, rms / std::max(1.0e-12, refRms)));
        auto* row = new juce::DynamicObject();
        row->setProperty("freq_hz", freqs[i]);
        row->setProperty("response_db_rel_500hz", relDb[i]);
        rows.add(juce::var(row));
    }

    // First -3 dB crossing above the reference, log-frequency interpolated.
    double cornerHz = 0.0;
    for (size_t i = 1; i < freqs.size(); ++i)
    {
        if (freqs[i] <= kReferenceHz || relDb[i] > -3.0)
            continue;
        const double dbA = relDb[i - 1];
        const double dbB = relDb[i];
        const double t = (dbA - (-3.0)) / std::max(1.0e-9, dbA - dbB);
        cornerHz = std::exp(std::log(freqs[i - 1]) + t * (std::log(freqs[i]) - std::log(freqs[i - 1])));
        break;
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "PreampInputLoadingCornerProbe");
    root->setProperty("schema", "spatial-echo.preamp-input-loading-corner-probe.v1");
    root->setProperty("configuration", "48kHz; PreampProcessor ONLY (no tape chain, no oversampling changes); "
                                       "drive=1.0; 50mFS sine per frequency, steady-state RMS vs 500Hz "
                                       "reference; corner = first -3dB crossing, log-f interpolated");
    root->setProperty("rows", rows);
    root->setProperty("corner_minus3db_hz", cornerHz);
    root->setProperty("input_loading_lpf_setting_hz", 5000.0);
    root->setProperty("finite", finite);

    const juce::File jsonFile = artifactDirectory().getChildFile("preamp_input_loading_corner_probe.json");
    jsonFile.deleteFile();
    const bool wroteJson = jsonFile.replaceWithText(juce::JSON::toString(juce::var(root), true));

    juce::String result;
    result << "PreampInputLoadingCornerProbe: corner=" << juce::String(cornerHz, 1)
           << "Hz (isolated preamp; lpFc setting 5000Hz)\n"
           << "[INFO] metrics: " << jsonFile.getFullPathName() << "\n"
           << ((finite && wroteJson && cornerHz > 0.0) ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::Preamp

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
