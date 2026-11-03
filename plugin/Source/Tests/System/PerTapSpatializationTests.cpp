// PerTapSpatializationTests: Per-tap spatial placement, metering, motion and the independent-voice renderer.

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../Common/TestUtilities.h"
#include "../../PluginProcessor.h"
#include "../../DSP/EchoesModel.h"
#include <JuceHeader.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>
#include <limits>

namespace SpatialEchoTests::System
{

// ============================================================================
// PerTapSpatialization: Real localization test with ITD measurement
// ============================================================================
juce::String runPerTapSpatialization(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int settleBlocks = 600;
    constexpr int captureBlocks = 200;
    constexpr float sineAmp = 0.1f;
    constexpr float sineFreqHz = 1000.0f;

    TapSpatialParams tapSpatial;
    tapSpatial.positionMode = 1;    // Independent
    tapSpatial.h1Azimuth = -90.0f;  // Left
    tapSpatial.h1Elevation = 0.0f;
    tapSpatial.h1Distance = 1.0f;
    tapSpatial.h2Azimuth = 90.0f;  // Right
    tapSpatial.h2Elevation = 0.0f;
    tapSpatial.h2Distance = 1.0f;
    tapSpatial.h3Azimuth = 0.0f;  // Center with elevation
    tapSpatial.h3Elevation = 40.0f;
    tapSpatial.h3Distance = 1.0f;

    int heads[3] = {1, 1, 1};

    std::vector<std::vector<float>> buffer(2, std::vector<float>((settleBlocks + captureBlocks) * blockSize, 0.0f));

    // Per-head accumulation buffers for measuring ITDs from actual arrivals
    std::array<std::vector<float>, 3> perHeadAccumulation;
    for (int h = 0; h < 3; ++h)
        perHeadAccumulation[static_cast<size_t>(h)].assign((settleBlocks + captureBlocks) * blockSize, 0.0f);

    EchoesModel model;
    model.Reset(fs, 2, blockSize);
    model.getSpatialField()->getVoiceProcessor(0)->useDefaultHRTF();

    model.getTapeDelay()->enablePerHeadCapture(true);

    // Stimulus: ONE short broadband noise burst, then silence. A continuous
    // sine is useless here: cross-correlation of a 1 kHz tone is ambiguous
    // modulo its 1000 us period, so ITD -479 us reads as +521 us and every
    // azimuth measures "identical". Broadband transients make ITD unambiguous.
    juce::Random stimulusRng(0x7A9);
    const int burstLenSamples = static_cast<int>(0.010f * fs);  // 10 ms
    juce::ignoreUnused(sineFreqHz);

    for (int b = 0; b < settleBlocks + captureBlocks; ++b)
    {
        std::vector<std::vector<float>> blockBuf(2, std::vector<float>(blockSize, 0.0f));

        for (int s = 0; s < blockSize; ++s)
        {
            const int globalSample = (b - settleBlocks) * blockSize + s;
            float v = 0.0f;
            if (globalSample >= 0 && globalSample < burstLenSamples)
                v = sineAmp * (stimulusRng.nextFloat() * 2.0f - 1.0f);
            blockBuf[0][static_cast<size_t>(s)] = v;
            blockBuf[1][static_cast<size_t>(s)] = v;
        }

        model.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, false, 80.0f, 160.0f, 240.0f, true, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f,
                               0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true,
                               1,  // spatialMode = Independent
                               1.0f, 0.0f, 0.0f, ReverbMacroParams{}, tapSpatial);

        model.ProcessBuffer(blockBuf, blockSize);

        for (int ch = 0; ch < 2; ++ch)
            std::copy(blockBuf[static_cast<size_t>(ch)].begin(), blockBuf[static_cast<size_t>(ch)].end(),
                      buffer[static_cast<size_t>(ch)].begin() + b * blockSize);

        // Accumulate per-head captures (each head processes L+R identically in spatial routing)
        // Copy each head's block-size capture into the full accumulation buffer
        for (int h = 0; h < 3; ++h)
        {
            const float* headOutput = model.getTapeDelay()->getHeadOutput(h);
            if (headOutput != nullptr)
            {
                std::copy(headOutput, headOutput + blockSize,
                          perHeadAccumulation[static_cast<size_t>(h)].begin() + b * blockSize);
            }
        }
    }

    // Extract windowed echoes and compute ITDs
    struct EchoMetrics
    {
        float itdMicroseconds = 0.0f;
        float peakL = 0.0f;
        float peakR = 0.0f;
        bool valid = true;
    };

    constexpr float h1DelayMs = 80.0f;
    constexpr float h2DelayMs = 160.0f;
    constexpr float h3DelayMs = 240.0f;
    constexpr int h1DelaySamples = static_cast<int>(h1DelayMs * fs / 1000.0f);
    constexpr int h2DelaySamples = static_cast<int>(h2DelayMs * fs / 1000.0f);
    constexpr int h3DelaySamples = static_cast<int>(h3DelayMs * fs / 1000.0f);

    // Helper: Find first arrival index in a per-head accumulated buffer
    // Returns -1 if no arrival detected
    auto findFirstArrival = [&](const std::vector<float>& headBuffer, int searchStartSample) -> int
    {
        constexpr float arrivalThreshold = 0.005f;
        for (int i = searchStartSample; i < (int)headBuffer.size(); ++i)
        {
            if (std::abs(headBuffer[static_cast<size_t>(i)]) > arrivalThreshold)
                return i;
        }
        return -1;
    };

    auto extractMetrics = [&](int delaySamples, int windowLenSamples) -> EchoMetrics
    {
        EchoMetrics m;
        if (delaySamples + windowLenSamples > (int)buffer[0].size())
            return m;

        std::vector<float> windowL(buffer[0].begin() + delaySamples,
                                   buffer[0].begin() + delaySamples + windowLenSamples);
        std::vector<float> windowR(buffer[1].begin() + delaySamples,
                                   buffer[1].begin() + delaySamples + windowLenSamples);

        m.peakL = *std::max_element(windowL.begin(), windowL.end(),
                                    [](float a, float b) { return std::abs(a) < std::abs(b); });
        m.peakR = *std::max_element(windowR.begin(), windowR.end(),
                                    [](float a, float b) { return std::abs(a) < std::abs(b); });

        int maxLag = static_cast<int>(0.001f * fs);
        int bestLag = 0;
        double bestCorr = -1e12;

        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double c = 0.0;
            int n = 0;
            for (int i = 0; i < (int)windowL.size() - std::abs(lag); ++i)
            {
                int iR = i + lag;
                if (iR >= 0 && iR < (int)windowR.size())
                {
                    c += static_cast<double>(windowL[static_cast<size_t>(i)]) *
                         static_cast<double>(windowR[static_cast<size_t>(iR)]);
                    ++n;
                }
            }
            if (n > 0)
                c /= static_cast<double>(n);
            if (c > bestCorr)
            {
                bestCorr = c;
                bestLag = lag;
            }
        }

        m.itdMicroseconds = (1000000.0f / fs) * static_cast<float>(bestLag);
        m.valid = std::isfinite(m.itdMicroseconds) && std::isfinite(m.peakL) && std::isfinite(m.peakR);
        return m;
    };

    constexpr int echoWindowSamples = static_cast<int>(0.05f * fs);
    constexpr int preWindowSamples = static_cast<int>(0.002f * fs);  // 2 ms before arrival

    // Search for actual first arrivals in each head's accumulated capture,
    // using expected delays as search-start hints
    int h1Arrival =
        findFirstArrival(perHeadAccumulation[0], settleBlocks * blockSize + h1DelaySamples - preWindowSamples);
    int h2Arrival =
        findFirstArrival(perHeadAccumulation[1], settleBlocks * blockSize + h2DelaySamples - preWindowSamples);
    int h3Arrival =
        findFirstArrival(perHeadAccumulation[2], settleBlocks * blockSize + h3DelaySamples - preWindowSamples);

    // Validate arrivals were found
    if (h1Arrival < 0)
        return "[FAIL] PerTapSpatialization: No H1 arrival detected in per-head capture";
    if (h2Arrival < 0)
        return "[FAIL] PerTapSpatialization: No H2 arrival detected in per-head capture";
    if (h3Arrival < 0)
        return "[FAIL] PerTapSpatialization: No H3 arrival detected in per-head capture";

    // Window around detected arrivals: [arrival - 2ms, arrival + 40ms]
    int h1Window = h1Arrival - preWindowSamples;
    int h2Window = h2Arrival - preWindowSamples;
    int h3Window = h3Arrival - preWindowSamples;

    // Ensure windows are valid
    if (h1Window < 0 || h1Window + echoWindowSamples > (int)buffer[0].size())
        return "[FAIL] PerTapSpatialization: H1 window out of bounds";
    if (h2Window < 0 || h2Window + echoWindowSamples > (int)buffer[0].size())
        return "[FAIL] PerTapSpatialization: H2 window out of bounds";
    if (h3Window < 0 || h3Window + echoWindowSamples > (int)buffer[0].size())
        return "[FAIL] PerTapSpatialization: H3 window out of bounds";

    auto h1 = extractMetrics(h1Window, echoWindowSamples);
    auto h2 = extractMetrics(h2Window, echoWindowSamples);
    auto h3 = extractMetrics(h3Window, echoWindowSamples);

    if (!h1.valid || !h2.valid || !h3.valid)
        return "[FAIL] PerTapSpatialization: ITD extraction produced non-finite values";

    const float itdDiff12 = std::abs(h1.itdMicroseconds - h2.itdMicroseconds);
    const float h1Mag = std::abs(h1.itdMicroseconds);
    const float h2Mag = std::abs(h2.itdMicroseconds);
    const float h3Mag = std::abs(h3.itdMicroseconds);

    if (std::abs(itdDiff12) < 100.0f)
    {
        return juce::String::formatted(
            "[FAIL] PerTapSpatialization: Independent mode not positioning heads correctly. "
            "H1 ITD=%.1f, H2 ITD=%.1f (should differ by >600us). DSP fix needed in EchoesModel spatial routing.",
            h1.itdMicroseconds, h2.itdMicroseconds);
    }

    const bool oppositeSigns = (h1.itdMicroseconds * h2.itdMicroseconds) < 0.0f;
    if (!oppositeSigns)
        return juce::String::formatted("[FAIL] PerTapSpatialization: H1/H2 ITDs not opposite sign (H1=%.1f, H2=%.1f)",
                                       h1.itdMicroseconds, h2.itdMicroseconds);

    if (h1Mag < 300.0f || h2Mag < 300.0f)
        return juce::String::formatted("[FAIL] PerTapSpatialization: H1/H2 magnitudes too small (H1=%.1f, H2=%.1f)",
                                       h1Mag, h2Mag);

    if (h3Mag > 150.0f)
        return juce::String::formatted("[FAIL] PerTapSpatialization: H3 ITD magnitude too large (%.1f > 150us)", h3Mag);

    return juce::String::formatted("[PASS] PerTapSpatialization: H1 ITD=%.1f us, H2 ITD=%.1f us, H3 ITD=%.1f us",
                                   h1.itdMicroseconds, h2.itdMicroseconds, h3.itdMicroseconds);
}

// ============================================================================
// Shared helpers for the processor-driven per-tap tests below
// ============================================================================
namespace
{

void setFloatParam(SpatialEchoAudioProcessor& processor, const char* id, float plainValue)
{
    auto& vts = processor.getValueTreeState();
    if (auto* param = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
        param->setValueNotifyingHost(param->convertTo0to1(plainValue));
}

void setChoiceParam(SpatialEchoAudioProcessor& processor, const char* id, int index)
{
    auto& vts = processor.getValueTreeState();
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
    {
        const int maxIdx = std::max(0, p->choices.size() - 1);
        const int clamped = juce::jlimit(0, maxIdx, index);
        const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
        p->setValueNotifyingHost(norm);
    }
}

void setBoolParam(SpatialEchoAudioProcessor& processor, const char* id, bool value)
{
    auto& vts = processor.getValueTreeState();
    if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
        p->setValueNotifyingHost(value ? 1.0f : 0.0f);
}

// Common config: Independent per-tap mode with two audible, hard-panned taps.
// DelaySetting 3 = "H2+H3 Echo" - the plugin default is H1-only, which would
// leave the constellation-offset taps (H2/H3) silent and gut these tests.
// Full-wet mix so the measured signal is the spatialized echo branch;
// wow/flutter off so repeated renders are deterministic.
void configurePerTapProcessor(SpatialEchoAudioProcessor& processor)
{
    setChoiceParam(processor, "DelaySetting", 3);      // H2+H3 Echo
    setChoiceParam(processor, "HeadPositionMode", 1);  // Independent
    setFloatParam(processor, "Head2Azimuth", -90.0f);  // H2 hard left
    setFloatParam(processor, "Head2Elevation", 0.0f);
    setFloatParam(processor, "Head2Distance", 1.0f);
    setFloatParam(processor, "Head3Azimuth", 90.0f);  // H3 hard right
    setFloatParam(processor, "Head3Elevation", 0.0f);
    setFloatParam(processor, "Head3Distance", 1.0f);
    setBoolParam(processor, "SpatialEnabled", true);
    setFloatParam(processor, "Mix", 1.0f);
    setFloatParam(processor, "WowFlutterDepth", 0.0f);
}

float computeWindowRms(const std::vector<float>& buf, int start, int end)
{
    double sum = 0.0;
    for (int i = start; i < end; ++i)
        sum += static_cast<double>(buf[static_cast<size_t>(i)]) * static_cast<double>(buf[static_cast<size_t>(i)]);
    const int count = end - start;
    return count > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
}

struct DirectRenderConfig
{
    bool independent = true;
    int spatialRoutingMode = 0;
    bool binauralEnabled = true;
    float binauralWetMix = 0.0f;
    float binauralCrossover = 150.0f;
    float sourceWidth = 0.5f;
    int binauralQuality = 2;
    float bass = 0.5f;
    float treble = 0.5f;
    int totalBlocks = 96;
    int burstSamples = 192;
    float burstAmplitude = 0.15f;
    float h1DelayMs = 45.0f;
    float h2DelayMs = 90.0f;
    float h3DelayMs = 135.0f;
    TapSpatialParams tapSpatial;
};

struct DirectRenderResult
{
    std::vector<std::vector<float>> output;
};

std::vector<std::vector<float>> makeStereoBurstInput(int totalSamples, int burstSamples, float amplitude,
                                                     float rightGain, float rightPolarity)
{
    std::vector<std::vector<float>> input(2, std::vector<float>(static_cast<size_t>(totalSamples), 0.0f));
    juce::Random rng(0x42C0FFEE);
    for (int i = 0; i < std::min(totalSamples, burstSamples); ++i)
    {
        const float sample = amplitude * (rng.nextFloat() * 2.0f - 1.0f);
        input[0][static_cast<size_t>(i)] = sample;
        input[1][static_cast<size_t>(i)] = sample * rightGain * rightPolarity;
    }
    return input;
}

DirectRenderResult renderDirectModel(const DirectRenderConfig& cfg, const std::vector<std::vector<float>>& input)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    const int totalSamples = cfg.totalBlocks * blockSize;

    DirectRenderResult result;
    result.output.assign(2, std::vector<float>(static_cast<size_t>(totalSamples), 0.0f));

    EchoesModel model;
    model.Reset(fs, 2, blockSize);
    if (auto* field = model.getSpatialField())
    {
        for (int v = 0; v < SpatialField::kMaxVoices; ++v)
            if (auto* voice = field->getVoiceProcessor(v))
                voice->useDefaultHRTF();
    }

    int heads[3] = {1, 1, 1};
    for (int block = 0; block < cfg.totalBlocks; ++block)
    {
        std::vector<std::vector<float>> blockBuf(2, std::vector<float>(blockSize, 0.0f));
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto offset = static_cast<size_t>(block * blockSize);
            std::copy_n(input[static_cast<size_t>(ch)].begin() + static_cast<long>(offset), blockSize,
                        blockBuf[static_cast<size_t>(ch)].begin());
        }

        TapSpatialParams tapSpatial = cfg.tapSpatial;
        tapSpatial.positionMode = cfg.independent ? 1 : 0;

        model.UpdateParameters(cfg.bass, cfg.treble, 0.45f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f,
                               0.0f, 0.5f, 0.5f, 0, true, cfg.h1DelayMs, cfg.h2DelayMs, cfg.h3DelayMs,
                               cfg.binauralEnabled, 0.0f, 0.0f, 1.0f, cfg.binauralWetMix, cfg.binauralCrossover,
                               cfg.binauralQuality, 0.5f, 0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true,
                               cfg.spatialRoutingMode, 1.0f, 0.0f, 0.0f, ReverbMacroParams{}, tapSpatial);

        if (auto* field = model.getSpatialField())
            field->setSourceWidth(cfg.sourceWidth * 100.0f);

        model.ProcessBuffer(blockBuf, blockSize);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto offset = static_cast<size_t>(block * blockSize);
            std::copy(blockBuf[static_cast<size_t>(ch)].begin(), blockBuf[static_cast<size_t>(ch)].end(),
                      result.output[static_cast<size_t>(ch)].begin() + static_cast<long>(offset));
        }
    }

    return result;
}

float computeStereoWindowRms(const std::vector<std::vector<float>>& buffer, int start, int end)
{
    double sum = 0.0;
    int count = 0;
    for (int ch = 0; ch < static_cast<int>(buffer.size()); ++ch)
    {
        for (int i = start; i < end; ++i)
        {
            const double value = buffer[static_cast<size_t>(ch)][static_cast<size_t>(i)];
            sum += value * value;
            ++count;
        }
    }
    return count > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
}

float computeStereoDiffDb(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b, int start,
                          int end)
{
    double num = 0.0;
    double den = 0.0;
    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = start; i < end; ++i)
        {
            const double x = a[static_cast<size_t>(ch)][static_cast<size_t>(i)];
            const double y = b[static_cast<size_t>(ch)][static_cast<size_t>(i)];
            const double d = x - y;
            num += d * d;
            den += x * x;
        }
    }

    if (den <= 1.0e-20)
        return -120.0f;
    return static_cast<float>(10.0 * std::log10(std::max(num / den, 1.0e-12)));
}

float computeStereoFinitePeak(const std::vector<std::vector<float>>& buffer)
{
    float peak = 0.0f;
    for (const auto& channel : buffer)
    {
        for (float sample : channel)
        {
            if (!std::isfinite(sample))
                return std::numeric_limits<float>::infinity();
            peak = std::max(peak, std::abs(sample));
        }
    }
    return peak;
}

float computeTrebleProxyDb(const std::vector<std::vector<float>>& buffer, int start, int end)
{
    double diffEnergy = 0.0;
    double totalEnergy = 0.0;
    int count = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        for (int i = std::max(start + 1, 1); i < end; ++i)
        {
            const double current = buffer[static_cast<size_t>(ch)][static_cast<size_t>(i)];
            const double previous = buffer[static_cast<size_t>(ch)][static_cast<size_t>(i - 1)];
            const double diff = current - previous;
            diffEnergy += diff * diff;
            totalEnergy += current * current;
            ++count;
        }
    }

    if (count <= 0 || totalEnergy <= 1.0e-20)
        return -120.0f;
    return static_cast<float>(10.0 * std::log10(std::max(diffEnergy / totalEnergy, 1.0e-12)));
}

}  // namespace

// ============================================================================
// PerTapMonoSafety: Verify per-tap spatialization survives mono downmix
// ============================================================================
juce::String runPerTapMonoSafety(SpatialEchoAudioProcessor& processor)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int settleBlocks = 600;
    constexpr int captureBlocks = 200;
    constexpr float stimulusAmp = 0.1f;

    processor.prepareToPlay(fs, blockSize);
    configurePerTapProcessor(processor);
    setChoiceParam(processor, "SpatialPattern", 0);  // Still (motion off)

    std::vector<std::vector<float>> buffer(2, std::vector<float>((settleBlocks + captureBlocks) * blockSize, 0.0f));
    juce::MidiBuffer midi;
    juce::Random stimulusRng(0x9F2);
    const int burstLenSamples = static_cast<int>(0.010f * fs);

    for (int b = 0; b < settleBlocks + captureBlocks; ++b)
    {
        juce::AudioBuffer<float> juceBuf(2, blockSize);
        for (int s = 0; s < blockSize; ++s)
        {
            const int globalSample = (b - settleBlocks) * blockSize + s;
            float v = 0.0f;
            if (globalSample >= 0 && globalSample < burstLenSamples)
                v = stimulusAmp * (stimulusRng.nextFloat() * 2.0f - 1.0f);
            juceBuf.setSample(0, s, v);
            juceBuf.setSample(1, s, v);
        }

        processor.processBlock(juceBuf, midi);

        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = juceBuf.getReadPointer(ch);
            std::copy(src, src + blockSize, buffer[static_cast<size_t>(ch)].begin() + b * blockSize);
        }
    }

    // Window [100ms, 600ms] after burst start - covers the H2 (~234 ms) and
    // H3 (~338 ms) echoes plus early repeats at the default repeat rate.
    constexpr int echoDlyMS = 100;
    constexpr int echoWindowMS = 500;
    const int echoStartSample = settleBlocks * blockSize + static_cast<int>(echoDlyMS * fs / 1000.0f);
    const int echoEndSample = echoStartSample + static_cast<int>(echoWindowMS * fs / 1000.0f);

    if (echoEndSample > (int)buffer[0].size())
        return "[FAIL] PerTapMonoSafety: Buffer too short";

    const float rmsL = computeWindowRms(buffer[0], echoStartSample, echoEndSample);
    const float rmsR = computeWindowRms(buffer[1], echoStartSample, echoEndSample);
    const float rmsAvg = 0.5f * (rmsL + rmsR);

    // Real mono downmix - the whole point of the test.
    double monoSum = 0.0;
    for (int i = echoStartSample; i < echoEndSample; ++i)
    {
        const double m = 0.5 * (static_cast<double>(buffer[0][static_cast<size_t>(i)]) +
                                static_cast<double>(buffer[1][static_cast<size_t>(i)]));
        monoSum += m * m;
    }
    const float rmsMono = static_cast<float>(std::sqrt(monoSum / static_cast<double>(echoEndSample - echoStartSample)));

    if (rmsAvg < 1e-6f)
        return "[FAIL] PerTapMonoSafety: Output is silent in echo window";

    bool identicalLR = true;
    for (int i = echoStartSample; i < echoEndSample; ++i)
    {
        if (std::abs(buffer[0][static_cast<size_t>(i)] - buffer[1][static_cast<size_t>(i)]) > 1e-6f)
        {
            identicalLR = false;
            break;
        }
    }
    if (identicalLR)
        return "[FAIL] PerTapMonoSafety: L and R channels are identical (no spatialization)";

    // PASS iff the mono downmix keeps its energy: within 6 dB of the stereo average.
    const float energyRatio = rmsMono / rmsAvg;
    const float dB = 20.0f * std::log10(std::max(energyRatio, 1e-6f));

    if (dB > -6.0f)
        return juce::String::formatted("[PASS] PerTapMonoSafety: mono/stereo-avg=%.2f dB (gate > -6 dB)", dB);

    return juce::String::formatted(
        juce::CharPointer_UTF8(
            "[FAIL] PerTapMonoSafety: mono/stereo-avg=%.2f dB (gate > -6 dB) — mono downmix collapses"),
        dB);
}

// ============================================================================
// IndependentStereoCaptureMatrix: preserve source stereo through independent taps
// ============================================================================
juce::String runIndependentStereoCaptureMatrixTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int blockSize = 256;
    constexpr int totalBlocks = 96;
    const int totalSamples = totalBlocks * blockSize;
    const int windowStart = static_cast<int>(0.040f * 48000.0f);
    const int windowEnd = static_cast<int>(0.260f * 48000.0f);

    DirectRenderConfig cfg;
    cfg.totalBlocks = totalBlocks;
    cfg.binauralWetMix = 0.0f;
    cfg.spatialRoutingMode = 0;
    cfg.tapSpatial.h1Azimuth = 0.0f;
    cfg.tapSpatial.h2Azimuth = 0.0f;
    cfg.tapSpatial.h3Azimuth = 0.0f;

    const auto correlatedInput = makeStereoBurstInput(totalSamples, cfg.burstSamples, cfg.burstAmplitude, 1.0f, 1.0f);
    const auto antiPhaseInput = makeStereoBurstInput(totalSamples, cfg.burstSamples, cfg.burstAmplitude, 1.0f, -1.0f);
    const auto rightOnlyInput = makeStereoBurstInput(totalSamples, cfg.burstSamples, cfg.burstAmplitude, 1.0f, 1.0f);

    auto rightOnlyStimulus = rightOnlyInput;
    std::fill(rightOnlyStimulus[0].begin(), rightOnlyStimulus[0].end(), 0.0f);

    const auto correlated = renderDirectModel(cfg, correlatedInput);
    const auto antiPhase = renderDirectModel(cfg, antiPhaseInput);
    const auto rightOnly = renderDirectModel(cfg, rightOnlyStimulus);

    const float rightOnlyRms = computeStereoWindowRms(rightOnly.output, windowStart, windowEnd);
    const float antiPhaseDiffDb = computeStereoDiffDb(correlated.output, antiPhase.output, windowStart, windowEnd);
    const float peakCorrelated = computeStereoFinitePeak(correlated.output);
    const float peakAntiPhase = computeStereoFinitePeak(antiPhase.output);
    const float peakRightOnly = computeStereoFinitePeak(rightOnly.output);

    if (!std::isfinite(peakCorrelated) || !std::isfinite(peakAntiPhase) || !std::isfinite(peakRightOnly))
        return "[FAIL] IndependentStereoCaptureMatrix: non-finite output";

    if (rightOnlyRms <= 1.0e-4f)
        return juce::String::formatted(
            "[FAIL] IndependentStereoCaptureMatrix: right-only independent echo is effectively silent (rms=%.6f)",
            rightOnlyRms);

    if (antiPhaseDiffDb <= -24.0f)
        return juce::String::formatted("[FAIL] IndependentStereoCaptureMatrix: anti-phase stereo collapsed toward "
                                       "correlated stereo (diff=%.2f dB)",
                                       antiPhaseDiffDb);

    return juce::String::formatted("[PASS] IndependentStereoCaptureMatrix: rightOnlyRms=%.6f antiPhaseDiff=%.2f dB",
                                   rightOnlyRms, antiPhaseDiffDb);
}

// ============================================================================
// IndependentPostTreatmentParity: linked and independent must share head treatment
// ============================================================================
juce::String runIndependentPostTreatmentParityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int totalBlocks = 96;
    const int totalSamples = totalBlocks * 256;
    const int windowStart = static_cast<int>(0.040f * 48000.0f);
    const int windowEnd = static_cast<int>(0.260f * 48000.0f);

    DirectRenderConfig linkedCfg;
    linkedCfg.totalBlocks = totalBlocks;
    linkedCfg.independent = false;
    linkedCfg.binauralWetMix = 100.0f;
    linkedCfg.binauralCrossover = 320.0f;
    linkedCfg.sourceWidth = 0.8f;
    linkedCfg.spatialRoutingMode = 0;
    linkedCfg.tapSpatial.h1Azimuth = 0.0f;
    linkedCfg.tapSpatial.h1Elevation = 0.0f;
    linkedCfg.tapSpatial.h1Distance = 1.0f;
    linkedCfg.tapSpatial.h2Azimuth = 0.0f;
    linkedCfg.tapSpatial.h2Elevation = 0.0f;
    linkedCfg.tapSpatial.h2Distance = 1.0f;
    linkedCfg.tapSpatial.h3Azimuth = 0.0f;
    linkedCfg.tapSpatial.h3Elevation = 0.0f;
    linkedCfg.tapSpatial.h3Distance = 1.0f;

    DirectRenderConfig independentCfg = linkedCfg;
    independentCfg.independent = true;

    const auto input = makeStereoBurstInput(totalSamples, linkedCfg.burstSamples, linkedCfg.burstAmplitude, 1.0f, 1.0f);
    const auto linked = renderDirectModel(linkedCfg, input);
    const auto independent = renderDirectModel(independentCfg, input);

    const float linkedRms = computeStereoWindowRms(linked.output, windowStart, windowEnd);
    const float independentRms = computeStereoWindowRms(independent.output, windowStart, windowEnd);
    const float diffDb = computeStereoDiffDb(linked.output, independent.output, windowStart, windowEnd);

    // Vacuity guard. computeStereoDiffDb() returns its -120 dB floor both for a
    // true null AND for a silent reference window (den <= 1e-20), so a render
    // that fell silent would otherwise report a perfect pass. Assert signal
    // first, then assert the null.
    if (linkedRms <= 1.0e-4f || independentRms <= 1.0e-4f)
        return juce::String::formatted("[FAIL] IndependentPostTreatmentParity: comparison window is effectively silent "
                                       "(linkedRms=%.7f independentRms=%.7f) - the null below would be vacuous",
                                       linkedRms, independentRms);

    // Threshold rationale (measured in Debug, 48 kHz/256):
    // With all three taps at the same position the two routes are the same
    // linear system evaluated in a different summation order, so they null to
    // the metric's -120 dB floor and the RMS values agree to 7 significant
    // figures (0.0291362 vs 0.0291362). Float summation-order noise alone lands
    // near -140 dB. The original -45 dB threshold was ~75 dB looser than the
    // real behaviour and would have accepted a gross routing gain error; -80 dB
    // still leaves 40 dB of margin over anything measured.
    if (diffDb > -80.0f)
        return juce::String::formatted("[FAIL] IndependentPostTreatmentParity: linked vs independent differ by %.2f dB "
                                       "through matched 100%%-wet renderers (linkedRms=%.7f independentRms=%.7f)",
                                       diffDb, linkedRms, independentRms);

    return juce::String::formatted("[PASS] IndependentPostTreatmentParity: linked vs independent diff=%.2f dB "
                                   "(linkedRms=%.7f independentRms=%.7f)",
                                   diffDb, linkedRms, independentRms);
}

// ============================================================================
// IndependentLevelParityProbe: INSTRUMENT, not a gate.
//
// Reports the Independent-vs-Linked level relationship across position,
// distance and width, plus a width energy/mono-fold sweep. It exists to make
// the numbers reproducible on demand; the actual regression gates are
// IndependentPostTreatmentParity (null) and SourceWidthEnergyConservation
// (width level law). It still fails on non-finite output so it cannot pass
// while the renderer is producing garbage. Registered as a measurement.
// ============================================================================
juce::String runIndependentLevelParityProbe(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int totalBlocks = 96;
    const int totalSamples = totalBlocks * 256;
    const int windowStart = static_cast<int>(0.040f * 48000.0f);
    const int windowEnd = static_cast<int>(0.260f * 48000.0f);

    const auto input = makeStereoBurstInput(totalSamples, 192, 0.15f, 1.0f, 1.0f);

    juce::String out;
    bool allFinite = true;

    auto probe = [&](const char* label, float az1, float az2, float az3, float dist, float width)
    {
        DirectRenderConfig linkedCfg;
        linkedCfg.totalBlocks = totalBlocks;
        linkedCfg.independent = false;
        linkedCfg.binauralWetMix = 100.0f;
        linkedCfg.binauralCrossover = 150.0f;
        linkedCfg.sourceWidth = width;
        linkedCfg.spatialRoutingMode = 0;
        linkedCfg.tapSpatial.h1Azimuth = az1;
        linkedCfg.tapSpatial.h2Azimuth = az2;
        linkedCfg.tapSpatial.h3Azimuth = az3;
        linkedCfg.tapSpatial.h1Distance = dist;
        linkedCfg.tapSpatial.h2Distance = dist;
        linkedCfg.tapSpatial.h3Distance = dist;

        DirectRenderConfig independentCfg = linkedCfg;
        independentCfg.independent = true;

        const auto linked = renderDirectModel(linkedCfg, input);
        const auto independent = renderDirectModel(independentCfg, input);

        if (!std::isfinite(computeStereoFinitePeak(linked.output)) ||
            !std::isfinite(computeStereoFinitePeak(independent.output)))
            allFinite = false;

        const float linkedRms = computeStereoWindowRms(linked.output, windowStart, windowEnd);
        const float independentRms = computeStereoWindowRms(independent.output, windowStart, windowEnd);
        const float diffDb = computeStereoDiffDb(linked.output, independent.output, windowStart, windowEnd);
        const float ratioDb = (linkedRms > 1.0e-12f && independentRms > 1.0e-12f)
                                  ? 20.0f * std::log10(independentRms / linkedRms)
                                  : -999.0f;

        out << "\n  " << label << " linkRms=" << juce::String(linkedRms, 7)
            << " indRms=" << juce::String(independentRms, 7) << " ind-vs-link=" << juce::String(ratioDb, 3) << "dB"
            << " null=" << juce::String(diffDb, 2) << "dB";
    };

    probe("samePos d1.0 w0.00", 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    probe("samePos d1.0 w0.50", 0.0f, 0.0f, 0.0f, 1.0f, 0.5f);
    probe("spread  d1.0 w0.00", -90.0f, 0.0f, 90.0f, 1.0f, 0.0f);
    probe("samePos d0.4 w0.00", 0.0f, 0.0f, 0.0f, 0.4f, 0.0f);
    probe("samePos d3.0 w0.00", 0.0f, 0.0f, 0.0f, 3.0f, 0.0f);

    // Width energy sweep on the Linked path (single voice; isolates the width
    // stage from any multi-voice effect). monoLoss is the L+R fold-down level
    // relative to the stereo level, i.e. how much the width stage costs a
    // mono listener on top of the stereo loss.
    auto widthProbe = [&](float width)
    {
        DirectRenderConfig cfg;
        cfg.totalBlocks = totalBlocks;
        cfg.independent = false;
        cfg.binauralWetMix = 100.0f;
        cfg.sourceWidth = width;
        cfg.spatialRoutingMode = 0;
        cfg.tapSpatial.h1Azimuth = 30.0f;
        cfg.tapSpatial.h2Azimuth = 30.0f;
        cfg.tapSpatial.h3Azimuth = 30.0f;

        const auto rendered = renderDirectModel(cfg, input);
        if (!std::isfinite(computeStereoFinitePeak(rendered.output)))
            allFinite = false;

        const float rms = computeStereoWindowRms(rendered.output, windowStart, windowEnd);

        double monoSum = 0.0;
        int monoCount = 0;
        for (int i = windowStart; i < windowEnd; ++i)
        {
            const double mono =
                0.5 * (rendered.output[0][static_cast<size_t>(i)] + rendered.output[1][static_cast<size_t>(i)]);
            monoSum += mono * mono;
            ++monoCount;
        }
        const float monoRms =
            (monoCount > 0) ? static_cast<float>(std::sqrt(monoSum / static_cast<double>(monoCount))) : 0.0f;

        out << "\n  width=" << juce::String(width, 2) << " rms=" << juce::String(rms, 7)
            << " monoRms=" << juce::String(monoRms, 7) << " monoLoss="
            << juce::String((rms > 1.0e-12f && monoRms > 1.0e-12f) ? 20.0f * std::log10(monoRms / rms) : -999.0f, 2)
            << "dB";
    };

    widthProbe(0.0f);
    widthProbe(0.25f);
    widthProbe(0.5f);
    widthProbe(0.75f);
    widthProbe(1.0f);

    if (!allFinite)
        return "[FAIL] IndependentLevelParityProbe: non-finite output in one or more renders" + out;

    return "[PASS] IndependentLevelParityProbe (instrument, thresholds live in the gates):" + out;
}

// ============================================================================
// SourceWidthEnergyConservation: Source Width must not act as a volume fader.
//
// Contract: Source Width repositions/spreads the image. Blending a focused
// HRIR toward a spread/diffuse one is standard, but the spread HRIR must be
// energy-normalised against the focused one and the blend must be
// power-complementary, otherwise the control silently attenuates the whole
// wet path. (Energy-conserving normalisation of the pseudo/diffuse HRTF
// weight is the standard construction for extended-source width rendering.)
//
// This asserts the wet-path level stays within +/-3 dB of the width=0
// reference across the whole control range. +/-3 dB is deliberately generous:
// a correct power-complementary blend of normalised IRs holds within ~1 dB.
//
// A width stage without that normalisation measured (48 kHz/256, single Linked
// voice at az=30): width 0.00 -> 0.0291362, 0.25 -> 0.0226861 (-2.17 dB),
// 0.50 -> 0.0155548 (-5.44 dB), 0.75 -> 0.0094218 (-9.81 dB),
// 1.00 -> 0.0059401 (-13.81 dB). The default width is 0.50, so such a stage
// puts the default patch 5.4 dB down on the spatial wet path.
// ============================================================================
juce::String runSourceWidthEnergyConservationTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int totalBlocks = 96;
    const int totalSamples = totalBlocks * 256;
    const int windowStart = static_cast<int>(0.040f * 48000.0f);
    const int windowEnd = static_cast<int>(0.260f * 48000.0f);
    constexpr float kToleranceDb = 3.0f;

    const auto input = makeStereoBurstInput(totalSamples, 192, 0.15f, 1.0f, 1.0f);

    auto renderAtWidth = [&](float width)
    {
        DirectRenderConfig cfg;
        cfg.totalBlocks = totalBlocks;
        cfg.independent = false;
        cfg.binauralWetMix = 100.0f;
        cfg.binauralCrossover = 150.0f;
        cfg.sourceWidth = width;
        cfg.spatialRoutingMode = 0;
        cfg.tapSpatial.h1Azimuth = 30.0f;
        cfg.tapSpatial.h2Azimuth = 30.0f;
        cfg.tapSpatial.h3Azimuth = 30.0f;
        return renderDirectModel(cfg, input);
    };

    const auto reference = renderAtWidth(0.0f);
    const float referenceRms = computeStereoWindowRms(reference.output, windowStart, windowEnd);

    // Vacuity guard: a silent reference would make every ratio meaningless.
    if (referenceRms <= 1.0e-4f)
        return juce::String::formatted("[FAIL] SourceWidthEnergyConservation: width=0 reference is silent (rms=%.7f)",
                                       referenceRms);

    const float widths[] = {0.25f, 0.5f, 0.75f, 1.0f};
    float worstDeviationDb = 0.0f;
    float worstWidth = 0.0f;
    juce::String detail;

    for (float width : widths)
    {
        const auto rendered = renderAtWidth(width);
        if (!std::isfinite(computeStereoFinitePeak(rendered.output)))
            return juce::String::formatted("[FAIL] SourceWidthEnergyConservation: non-finite output at width=%.2f",
                                           width);

        const float rms = computeStereoWindowRms(rendered.output, windowStart, windowEnd);
        const float deltaDb = (rms > 1.0e-12f) ? 20.0f * std::log10(rms / referenceRms) : -120.0f;

        detail << " w" << juce::String(width, 2) << "=" << juce::String(deltaDb, 2) << "dB";

        if (std::abs(deltaDb) > std::abs(worstDeviationDb))
        {
            worstDeviationDb = deltaDb;
            worstWidth = width;
        }
    }

    if (std::abs(worstDeviationDb) > kToleranceDb)
        return juce::String::formatted("[FAIL] SourceWidthEnergyConservation: width acts as a level control - "
                                       "worst %.2f dB at width=%.2f (tolerance +/-%.1f dB, ref rms=%.7f)%s",
                                       worstDeviationDb, worstWidth, kToleranceDb, referenceRms, detail.toRawUTF8());

    return juce::String::formatted("[PASS] SourceWidthEnergyConservation: worst %.2f dB at width=%.2f%s",
                                   worstDeviationDb, worstWidth, detail.toRawUTF8());
}

// ============================================================================
// IndependentEnvelopeAuthority: captured taps own the same enable/kill fades
// as the Linked output, rather than bypassing those audible envelopes.
// ============================================================================
juce::String runIndependentEnvelopeAuthorityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int totalBlocks = 128;

    TapeDelayProcessor delay;
    delay.Reset(fs, 2, blockSize);
    delay.enablePerHeadCapture(true);
    int heads[3] = {1, 1, 1};

    auto update = [&](bool enabled)
    {
        delay.UpdateParameters(0.72f, 0.5f, heads, enabled ? 1 : 0, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true,
                               45.0f, 90.0f, 135.0f, 0, 0, true, -1.0f, -1.0f, 1.0f, 1.0f);
    };

    update(true);
    float maxDiff = 0.0f;
    float maxOutput = 0.0f;
    float transitionEnergy = 0.0f;

    for (int blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
    {
        if (blockIndex == 56)
            update(false);
        if (blockIndex == 72)
            update(true);
        if (blockIndex == 96)
            delay.triggerKill();

        std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));
        if (blockIndex == 0 || blockIndex == 80)
        {
            block[0][0] = 0.25f;
            block[1][0] = -0.20f;
        }

        delay.ProcessBuffer(block, blockSize);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* h1 = delay.getHeadOutput(0, ch);
            const float* h2 = delay.getHeadOutput(1, ch);
            const float* h3 = delay.getHeadOutput(2, ch);
            if (!h1 || !h2 || !h3)
                return "[FAIL] IndependentEnvelopeAuthority: missing stereo head capture";

            for (int sample = 0; sample < blockSize; ++sample)
            {
                const float reconstructed = h1[sample] + h2[sample] + h3[sample];
                maxDiff = std::max(
                    maxDiff, std::abs(block[static_cast<size_t>(ch)][static_cast<size_t>(sample)] - reconstructed));
                maxOutput = std::max(maxOutput, std::abs(block[static_cast<size_t>(ch)][static_cast<size_t>(sample)]));
                if ((blockIndex >= 56 && blockIndex < 80) || blockIndex >= 96)
                    transitionEnergy = std::max(transitionEnergy, std::abs(reconstructed));
            }
        }
    }

    const bool passed = maxOutput > 1.0e-4f && transitionEnergy > 1.0e-6f && maxDiff <= 1.0e-6f;
    return juce::String::formatted("%s IndependentEnvelopeAuthority: maxDiff=%.9f peak=%.6f transition=%.6f",
                                   passed ? "[PASS]" : "[FAIL]", maxDiff, maxOutput, transitionEnergy);
}

// ============================================================================
// IndependentToneAuthority: tone must affect linked and independent equally
// ============================================================================
juce::String runIndependentToneAuthorityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int totalBlocks = 96;
    const int totalSamples = totalBlocks * 256;
    const int windowStart = static_cast<int>(0.040f * 48000.0f);
    const int windowEnd = static_cast<int>(0.260f * 48000.0f);

    DirectRenderConfig linkedNeutral;
    linkedNeutral.totalBlocks = totalBlocks;
    linkedNeutral.independent = false;
    linkedNeutral.binauralWetMix = 0.0f;
    linkedNeutral.treble = 0.5f;

    DirectRenderConfig linkedBright = linkedNeutral;
    linkedBright.treble = 1.0f;

    DirectRenderConfig independentNeutral = linkedNeutral;
    independentNeutral.independent = true;

    DirectRenderConfig independentBright = independentNeutral;
    independentBright.treble = 1.0f;

    const auto input =
        makeStereoBurstInput(totalSamples, linkedNeutral.burstSamples, linkedNeutral.burstAmplitude, 1.0f, 1.0f);

    const auto linkedA = renderDirectModel(linkedNeutral, input);
    const auto linkedB = renderDirectModel(linkedBright, input);
    const auto independentA = renderDirectModel(independentNeutral, input);
    const auto independentB = renderDirectModel(independentBright, input);

    const float linkedTrebleA = computeTrebleProxyDb(linkedA.output, windowStart, windowEnd);
    const float linkedTrebleB = computeTrebleProxyDb(linkedB.output, windowStart, windowEnd);
    const float independentTrebleA = computeTrebleProxyDb(independentA.output, windowStart, windowEnd);
    const float independentTrebleB = computeTrebleProxyDb(independentB.output, windowStart, windowEnd);

    const float linkedDelta = linkedTrebleB - linkedTrebleA;
    const float independentDelta = independentTrebleB - independentTrebleA;
    const float mismatch = std::abs(linkedDelta - independentDelta);

    if (std::abs(linkedDelta) < 1.0f)
        return juce::String::formatted("[FAIL] IndependentToneAuthority: linked treble delta too small (%.2f dB proxy)",
                                       linkedDelta);

    if (std::abs(independentDelta) < 1.0f || mismatch > 1.5f)
        return juce::String::formatted("[FAIL] IndependentToneAuthority: linked delta %.2f dB proxy, independent delta "
                                       "%.2f dB proxy, mismatch %.2f dB",
                                       linkedDelta, independentDelta, mismatch);

    return juce::String::formatted(
        "[PASS] IndependentToneAuthority: linked delta %.2f dB proxy, independent delta %.2f dB proxy", linkedDelta,
        independentDelta);
}

// ============================================================================
// IndependentSpatialExactOnce: before/after choice must not change independent echo
// ============================================================================
juce::String runIndependentSpatialExactOnceTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int totalBlocks = 96;
    const int totalSamples = totalBlocks * 256;
    const int windowStart = static_cast<int>(0.040f * 48000.0f);
    const int windowEnd = static_cast<int>(0.260f * 48000.0f);

    DirectRenderConfig afterCfg;
    afterCfg.totalBlocks = totalBlocks;
    afterCfg.independent = true;
    afterCfg.spatialRoutingMode = 0;
    afterCfg.binauralWetMix = 100.0f;
    afterCfg.tapSpatial.h1Azimuth = -80.0f;
    afterCfg.tapSpatial.h2Azimuth = 80.0f;
    afterCfg.tapSpatial.h3Azimuth = 0.0f;
    afterCfg.tapSpatial.h3Elevation = 30.0f;

    DirectRenderConfig beforeCfg = afterCfg;
    beforeCfg.spatialRoutingMode = 1;

    const auto input = makeStereoBurstInput(totalSamples, afterCfg.burstSamples, afterCfg.burstAmplitude, 1.0f, 1.0f);
    const auto after = renderDirectModel(afterCfg, input);
    const auto before = renderDirectModel(beforeCfg, input);

    const float diffDb = computeStereoDiffDb(after.output, before.output, windowStart, windowEnd);
    if (diffDb > -45.0f)
        return juce::String::formatted("[FAIL] IndependentSpatialExactOnce: independent output changes when Before "
                                       "Effects is selected (diff=%.2f dB)",
                                       diffDb);

    return juce::String::formatted("[PASS] IndependentSpatialExactOnce: before-vs-after diff=%.2f dB", diffDb);
}

// ============================================================================
// AllVoiceSpatialConfigPropagation: global spatial config must reach all voices
// ============================================================================
juce::String runAllVoiceSpatialConfigPropagationTest(SpatialEchoAudioProcessor& processor)
{
    constexpr float expectedWet = 0.375f;
    EchoesModel model;
    model.Reset(48000.0f, 2, 256);

    TapSpatialParams tapSpatial;
    tapSpatial.positionMode = 1;

    int heads[3] = {1, 1, 1};
    model.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f,
                           0, true, 45.0f, 90.0f, 135.0f, true, 23.0f, 12.0f, 2.5f, 37.5f, 777.0f, 1, 0.5f, 0.5f, 0.5f,
                           0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 0.0f, 0.0f, ReverbMacroParams{}, tapSpatial);

    auto* field = model.getSpatialField();
    if (!field)
        return "[FAIL] AllVoiceSpatialConfigPropagation: missing spatial field";

    field->setDiffuseFieldEQEnabled(false);
    field->setCrossoverFrequency(333.0f);

    for (int v = 0; v < SpatialField::kMaxVoices; ++v)
    {
        auto* voice = field->getVoiceProcessor(v);
        if (!voice)
            return juce::String::formatted("[FAIL] AllVoiceSpatialConfigPropagation: missing voice %d", v);

        if (!voice->getEnabled())
            return juce::String::formatted("[FAIL] AllVoiceSpatialConfigPropagation: voice %d not enabled", v);

        if (std::abs(voice->getWetMix() - expectedWet) > 0.01f)
            return juce::String::formatted("[FAIL] AllVoiceSpatialConfigPropagation: voice %d wet mix %.2f != %.2f", v,
                                           voice->getWetMix(), expectedWet);

        if (voice->getHRTFQuality() != BinauralProcessor::HRTFQuality::Balanced)
            return juce::String::formatted("[FAIL] AllVoiceSpatialConfigPropagation: voice %d quality mismatch", v);

        if (voice->getDiffuseFieldEQEnabled())
            return juce::String::formatted("[FAIL] AllVoiceSpatialConfigPropagation: voice %d diffuse EQ mismatch", v);

        if (std::abs(voice->getCrossoverFrequency() - 333.0f) > 0.1f)
            return juce::String::formatted("[FAIL] AllVoiceSpatialConfigPropagation: voice %d crossover mismatch", v);
    }

    processor.prepareToPlay(48000.0, 256);
    setFloatParam(processor, "SpatialSourceWidth", 0.73f);
    juce::AudioBuffer<float> silent(2, 256);
    silent.clear();
    juce::MidiBuffer midi;
    processor.processBlock(silent, midi);

    auto* processorField = processor.getSpatialFieldForTest();
    if (!processorField)
        return "[FAIL] AllVoiceSpatialConfigPropagation: processor field missing";
    for (int v = 0; v < SpatialField::kMaxVoices; ++v)
    {
        const auto* voice = processorField->getVoiceProcessor(v);
        if (!voice || std::abs(voice->getSourceWidth() - 73.0f) > 0.1f)
            return juce::String::formatted(
                "[FAIL] AllVoiceSpatialConfigPropagation: processor source width missing on voice %d", v);
    }

    return "[PASS] AllVoiceSpatialConfigPropagation: all renderer controls match on every voice";
}

// ============================================================================
// IndependentVoiceHistoryReentry: route changes must not revive stale HRIR tails
// ============================================================================
juce::String runIndependentVoiceHistoryReentryTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int warmBlocks = 48;
    constexpr int captureBlocks = 4;

    EchoesModel exercised;
    EchoesModel freshReference;
    exercised.Reset(fs, 2, blockSize);
    freshReference.Reset(fs, 2, blockSize);

    TapSpatialParams independent;
    independent.positionMode = 1;
    independent.h1Azimuth = -75.0f;
    independent.h2Azimuth = 70.0f;
    independent.h3Azimuth = 10.0f;
    independent.h3Elevation = 35.0f;

    // Give the clean reference the same active HRIRs as the exercised model
    // will have after its warm-up, but no convolution/delay history. Otherwise
    // this test measures a cold HRIR transition instead of stale audio state.
    if (auto* field = freshReference.getSpatialField())
    {
        const float azimuths[3] = {independent.h1Azimuth, independent.h2Azimuth, independent.h3Azimuth};
        const float elevations[3] = {independent.h1Elevation, independent.h2Elevation, independent.h3Elevation};
        const float distances[3] = {independent.h1Distance, independent.h2Distance, independent.h3Distance};
        for (int headIdx = 0; headIdx < SpatialField::kTapVoiceCount; ++headIdx)
        {
            if (auto* voice = field->getVoiceProcessor(SpatialField::kTapVoiceOffset + headIdx))
            {
                voice->snapToPosition(azimuths[headIdx], elevations[headIdx], distances[headIdx]);
                voice->primeSpatialStateForTest();
                voice->reset();
            }
        }
    }

    int heads[3] = {1, 1, 1};
    auto configure = [&](EchoesModel& model, bool independentMode, bool binauralEnabled)
    {
        auto tap = independent;
        tap.positionMode = independentMode ? 1 : 0;
        model.UpdateParameters(0.5f, 0.5f, 0.55f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, true, 45.0f, 90.0f, 135.0f, binauralEnabled, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f,
                               2, 0.5f, 0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 0.0f, 0.0f,
                               ReverbMacroParams{}, tap);
    };

    juce::Random rng(0x51A1E);
    for (int block = 0; block < warmBlocks; ++block)
    {
        std::vector<std::vector<float>> input(2, std::vector<float>(blockSize, 0.0f));
        if (block == 0)
        {
            for (int s = 0; s < 192; ++s)
            {
                const float sample = 0.15f * (rng.nextFloat() * 2.0f - 1.0f);
                input[0][static_cast<size_t>(s)] = sample;
                input[1][static_cast<size_t>(s)] = sample;
            }
        }

        auto referenceInput = input;
        configure(exercised, true, true);
        configure(freshReference, true, false);
        exercised.ProcessBuffer(input, blockSize);
        freshReference.ProcessBuffer(referenceInput, blockSize);
    }

    // Leave Independent for one block. Only the exercised model has spatial
    // history to flush; the tape and tone paths remain sample-identical.
    std::vector<std::vector<float>> transition(2, std::vector<float>(blockSize, 0.0f));
    auto referenceTransition = transition;
    configure(exercised, false, true);
    configure(freshReference, true, false);
    exercised.ProcessBuffer(transition, blockSize);
    freshReference.ProcessBuffer(referenceTransition, blockSize);

    // Match the incoming renderer's control state before clearing only the
    // reference histories. The exercised tap voices were cleared by the
    // Independent -> Linked transition while still enabled; resetting a
    // disabled reference would test the enable smoother instead of history.
    configure(freshReference, true, true);
    if (auto* field = freshReference.getSpatialField())
    {
        const float azimuths[3] = {independent.h1Azimuth, independent.h2Azimuth, independent.h3Azimuth};
        const float elevations[3] = {independent.h1Elevation, independent.h2Elevation, independent.h3Elevation};
        const float distances[3] = {independent.h1Distance, independent.h2Distance, independent.h3Distance};
        for (int headIdx = 0; headIdx < SpatialField::kTapVoiceCount; ++headIdx)
        {
            if (auto* voice = field->getVoiceProcessor(SpatialField::kTapVoiceOffset + headIdx))
            {
                voice->snapToPosition(azimuths[headIdx], elevations[headIdx], distances[headIdx]);
                voice->primeSpatialStateForTest();
                voice->reset();
            }
        }
    }

    std::vector<std::vector<float>> exercisedCapture(2, std::vector<float>(captureBlocks * blockSize, 0.0f));
    std::vector<std::vector<float>> referenceCapture(2, std::vector<float>(captureBlocks * blockSize, 0.0f));

    for (int block = 0; block < captureBlocks; ++block)
    {
        std::vector<std::vector<float>> a(2, std::vector<float>(blockSize, 0.0f));
        std::vector<std::vector<float>> b = a;
        configure(exercised, true, true);
        configure(freshReference, true, true);
        exercised.ProcessBuffer(a, blockSize);
        freshReference.ProcessBuffer(b, blockSize);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto offset = static_cast<size_t>(block * blockSize);
            std::copy(a[static_cast<size_t>(ch)].begin(), a[static_cast<size_t>(ch)].end(),
                      exercisedCapture[static_cast<size_t>(ch)].begin() + static_cast<long>(offset));
            std::copy(b[static_cast<size_t>(ch)].begin(), b[static_cast<size_t>(ch)].end(),
                      referenceCapture[static_cast<size_t>(ch)].begin() + static_cast<long>(offset));
        }
    }

    const float outputRms = computeStereoWindowRms(referenceCapture, 0, captureBlocks * blockSize);
    const float diffDb = computeStereoDiffDb(exercisedCapture, referenceCapture, 0, captureBlocks * blockSize);

    if (outputRms <= 1.0e-5f)
        return "[FAIL] IndependentVoiceHistoryReentry: comparison window is silent";
    if (diffDb > -60.0f)
        return juce::String::formatted(
            "[FAIL] IndependentVoiceHistoryReentry: stale voice history changed re-entry by %.2f dB", diffDb);

    // Exercise the reverse direction as well. Voice 0 owns Linked rendering
    // and must be cleared when it becomes dormant on entry to Independent.
    EchoesModel linkedExercised;
    EchoesModel linkedReference;
    linkedExercised.Reset(fs, 2, blockSize);
    linkedReference.Reset(fs, 2, blockSize);

    juce::Random linkedRng(0x711CED);
    for (int block = 0; block < warmBlocks; ++block)
    {
        std::vector<std::vector<float>> input(2, std::vector<float>(blockSize, 0.0f));
        if (block == 0)
        {
            for (int sampleIndex = 0; sampleIndex < 192; ++sampleIndex)
            {
                const float sample = 0.15f * (linkedRng.nextFloat() * 2.0f - 1.0f);
                input[0][static_cast<size_t>(sampleIndex)] = sample;
                input[1][static_cast<size_t>(sampleIndex)] = sample;
            }
        }

        auto referenceInput = input;
        configure(linkedExercised, false, true);
        configure(linkedReference, false, false);
        linkedExercised.ProcessBuffer(input, blockSize);
        linkedReference.ProcessBuffer(referenceInput, blockSize);
    }

    std::vector<std::vector<float>> linkedTransition(2, std::vector<float>(blockSize, 0.0f));
    auto linkedReferenceTransition = linkedTransition;
    configure(linkedExercised, true, true);
    configure(linkedReference, false, false);
    linkedExercised.ProcessBuffer(linkedTransition, blockSize);
    linkedReference.ProcessBuffer(linkedReferenceTransition, blockSize);

    configure(linkedReference, false, true);
    if (auto* field = linkedReference.getSpatialField())
    {
        if (auto* voice = field->getVoiceProcessor(SpatialField::kGlobalVoiceIndex))
        {
            voice->snapToPosition(0.0f, 0.0f, 1.0f);
            voice->primeSpatialStateForTest();
            voice->reset();
        }
    }

    std::vector<std::vector<float>> linkedCapture(2, std::vector<float>(captureBlocks * blockSize, 0.0f));
    std::vector<std::vector<float>> linkedReferenceCapture(2, std::vector<float>(captureBlocks * blockSize, 0.0f));
    for (int block = 0; block < captureBlocks; ++block)
    {
        std::vector<std::vector<float>> a(2, std::vector<float>(blockSize, 0.0f));
        std::vector<std::vector<float>> b = a;
        configure(linkedExercised, false, true);
        configure(linkedReference, false, true);
        linkedExercised.ProcessBuffer(a, blockSize);
        linkedReference.ProcessBuffer(b, blockSize);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto offset = static_cast<size_t>(block * blockSize);
            std::copy(a[static_cast<size_t>(ch)].begin(), a[static_cast<size_t>(ch)].end(),
                      linkedCapture[static_cast<size_t>(ch)].begin() + static_cast<long>(offset));
            std::copy(b[static_cast<size_t>(ch)].begin(), b[static_cast<size_t>(ch)].end(),
                      linkedReferenceCapture[static_cast<size_t>(ch)].begin() + static_cast<long>(offset));
        }
    }

    const float linkedOutputRms = computeStereoWindowRms(linkedReferenceCapture, 0, captureBlocks * blockSize);
    const float linkedDiffDb = computeStereoDiffDb(linkedCapture, linkedReferenceCapture, 0, captureBlocks * blockSize);
    if (linkedOutputRms <= 1.0e-5f)
        return "[FAIL] IndependentVoiceHistoryReentry: linked comparison window is silent";
    if (linkedDiffDb > -60.0f)
        return juce::String::formatted(
            "[FAIL] IndependentVoiceHistoryReentry: stale global history changed re-entry by %.2f dB", linkedDiffDb);

    return juce::String::formatted(
        "[PASS] IndependentVoiceHistoryReentry: tapDiff=%.2f dB linkedDiff=%.2f dB rms=%.6f/%.6f", diffDb, linkedDiffDb,
        outputRms, linkedOutputRms);
}

// ============================================================================
// PerTapMotionConstellation: Prove Constellation mode changes sound vs Unison
// ============================================================================
juce::String runPerTapMotionConstellation(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int settleBlocks = 600;
    constexpr int captureBlocks = 200;
    constexpr float stimulusAmp = 0.1f;

    // Render one pass with a given MotionTapMode on a fresh processor instance.
    auto renderPass = [&](int motionTapMode) -> std::vector<std::vector<float>>
    {
        SpatialEchoAudioProcessor passProc;
        passProc.prepareToPlay(fs, blockSize);
        configurePerTapProcessor(passProc);
        setChoiceParam(passProc, "SpatialPattern", 1);  // Orbit pattern enables motion
        setFloatParam(passProc, "SpatialRate", 1.0f);
        setFloatParam(passProc, "SpatialDepth", 60.0f);
        setFloatParam(passProc, "SpatialSpread", 100.0f);
        setChoiceParam(passProc, "SpatialChoreography", motionTapMode);

        std::vector<std::vector<float>> buf(2, std::vector<float>((settleBlocks + captureBlocks) * blockSize, 0.0f));
        juce::MidiBuffer midi;
        juce::Random rng(0x5A7);
        const int burstLenSamples = static_cast<int>(0.010f * fs);

        for (int b = 0; b < settleBlocks + captureBlocks; ++b)
        {
            juce::AudioBuffer<float> juceBuf(2, blockSize);
            for (int s = 0; s < blockSize; ++s)
            {
                const int globalSample = (b - settleBlocks) * blockSize + s;
                float v = 0.0f;
                if (globalSample >= 0 && globalSample < burstLenSamples)
                    v = stimulusAmp * (rng.nextFloat() * 2.0f - 1.0f);
                juceBuf.setSample(0, s, v);
                juceBuf.setSample(1, s, v);
            }

            passProc.processBlock(juceBuf, midi);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float* src = juceBuf.getReadPointer(ch);
                std::copy(src, src + blockSize, buf[static_cast<size_t>(ch)].begin() + b * blockSize);
            }
        }
        return buf;
    };

    auto bufA = renderPass(0);  // Unison
    auto bufB = renderPass(1);  // Constellation
    auto bufC = renderPass(0);  // Unison again - control for cross-instance nondeterminism

    const int start = settleBlocks * blockSize;
    const int end = (settleBlocks + captureBlocks) * blockSize;

    const float rmsA = computeWindowRms(bufA[0], start, end) + computeWindowRms(bufA[1], start, end);
    const float rmsB = computeWindowRms(bufB[0], start, end) + computeWindowRms(bufB[1], start, end);
    if (rmsA < 1e-6f || rmsB < 1e-6f)
        return juce::String::formatted(
            "[FAIL] PerTapMotionConstellation: One or both passes silent (rmsA=%.2e, rmsB=%.2e)", rmsA, rmsB);

    // Difference energy relative to reference energy, over both channels.
    auto diffDb = [&](const std::vector<std::vector<float>>& x, const std::vector<std::vector<float>>& y) -> float
    {
        double num = 0.0, den = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = start; i < end; ++i)
            {
                const double d = static_cast<double>(x[static_cast<size_t>(ch)][static_cast<size_t>(i)]) -
                                 static_cast<double>(y[static_cast<size_t>(ch)][static_cast<size_t>(i)]);
                num += d * d;
                den += static_cast<double>(x[static_cast<size_t>(ch)][static_cast<size_t>(i)]) *
                       static_cast<double>(x[static_cast<size_t>(ch)][static_cast<size_t>(i)]);
            }
        }
        if (den < 1e-20)
            return 0.0f;
        return static_cast<float>(10.0 * std::log10(std::max(num / den, 1e-12)));
    };

    const float dAB = diffDb(bufA, bufB);  // Unison vs Constellation (the effect)
    const float dAC = diffDb(bufA, bufC);  // Unison vs Unison (the noise floor)

    // PASS iff the constellation effect is audible AND clearly above whatever
    // run-to-run nondeterminism the engine has (control margin >= 12 dB).
    if (dAB > -40.0f && (dAB - dAC) >= 12.0f)
        return juce::String::formatted("[PASS] PerTapMotionConstellation: Unison-vs-Constellation %.2f dB, control "
                                       "floor %.2f dB (gates: > -40 dB and >= floor+12 dB)",
                                       dAB, dAC);

    return juce::String::formatted("[FAIL] PerTapMotionConstellation: Unison-vs-Constellation %.2f dB, control floor "
                                   "%.2f dB (gates: > -40 dB and >= floor+12 dB)",
                                   dAB, dAC);
}

// ============================================================================
// PerTapMotionSmoothness: Verify per-tap motion doesn't click
// ============================================================================
juce::String runPerTapMotionSmoothness(SpatialEchoAudioProcessor& processor)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int numBlocks = 2400;  // ~12.8 s at 48 kHz / 256-sample blocks
    constexpr float stimulusAmp = 0.25f;
    constexpr float sineFreqHz = 220.0f;

    // Isolation probe: SPATIAL_ECHO_SMOOTH_PROBE=unit feeds the sine straight
    // into a standalone BinauralProcessor at a static position - no delay, no
    // plugin - to test the core renderer's continuity by itself.
    if (const char* probeEnv = std::getenv("SPATIAL_ECHO_SMOOTH_PROBE");
        probeEnv != nullptr && std::strcmp(probeEnv, "unit") == 0)
    {
        BinauralProcessor binaural;
        if (std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_REPREPARE") != nullptr)
            binaural.prepare(48000.0, 512, 2);  // mimic host preparing large, then smaller
        int prepareSize = blockSize;
        if (const char* ps = std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_PREPARE"))
            prepareSize = juce::jlimit(16, 65536, atoi(ps));  // mimic plugin safeBlockSize
        int feedSize = blockSize;
        if (const char* fs2 = std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_FEED"))
            feedSize = juce::jlimit(16, prepareSize, atoi(fs2));
        binaural.prepare(48000.0, prepareSize, 2);
        binaural.useDefaultHRTF();
        binaural.setEnabled(true);
        binaural.setWetMix(100.0f);
        binaural.snapToPosition(-90.0f, 0.0f, 1.0f);
        if (const char* w = std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_WIDTH"))
            binaural.setSourceWidth((float)std::atof(w));
        if (std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_DIAG") != nullptr)
            binaural.setDiagnosticsEnabled(true);  // EchoesModel enables this on voice 0 in debug builds

        juce::AudioBuffer<float> block(2, feedSize);
        const int unitNumBlocks = (numBlocks * blockSize) / feedSize;
        std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<size_t>(unitNumBlocks * feedSize), 0.0f));
        double ph = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * sineFreqHz / fs;
        double totalProcessUs = 0.0;
        const bool perBlockSetters = std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_SETTERS") != nullptr;

        // Optional: replace the sine stimulus with recorded audio (raw float32,
        // one file per channel: <prefix>_ch0.f32 / <prefix>_ch1.f32).
        std::vector<std::vector<float>> extInput;
        if (const char* inPrefix = std::getenv("SPATIAL_ECHO_SMOOTH_UNIT_INPUT"))
        {
            extInput.resize(2);
            for (int ch = 0; ch < 2; ++ch)
            {
                juce::File f(juce::String(inPrefix) + "_ch" + juce::String(ch) + ".f32");
                juce::FileInputStream is(f);
                if (is.openedOk())
                {
                    extInput[static_cast<size_t>(ch)].resize((size_t)(is.getTotalLength() / (juce::int64)sizeof(float)),
                                                             0.0f);
                    is.read(extInput[static_cast<size_t>(ch)].data(),
                            (int)(extInput[static_cast<size_t>(ch)].size() * sizeof(float)));
                }
            }
            if (extInput[0].empty() || extInput[1].empty())
                return "[PROBE] unit: failed to read external input files";
        }

        for (int b = 0; b < unitNumBlocks; ++b)
        {
            if (perBlockSetters)
            {
                // Mimic EchoesModel::UpdateParameters - same setters, every block.
                binaural.setEnabled(true);
                binaural.setAzimuth(-90.0f);
                binaural.setElevation(0.0f);
                binaural.setDistance(1.0f);
                binaural.setWetMix(100.0f);
                binaural.setCrossoverFrequency(150.0f);
                binaural.setHRTFQuality(BinauralProcessor::HRTFQuality::Quality);
                binaural.setHrirInterpolationEnabledForTest(true);
            }
            for (int s = 0; s < feedSize; ++s)
            {
                if (!extInput.empty())
                {
                    const size_t gi = (size_t)b * (size_t)feedSize + (size_t)s;
                    block.setSample(0, s, gi < extInput[0].size() ? extInput[0][gi] : 0.0f);
                    block.setSample(1, s, gi < extInput[1].size() ? extInput[1][gi] : 0.0f);
                }
                else
                {
                    const float v = stimulusAmp * static_cast<float>(std::sin(ph));
                    ph += inc;
                    if (ph > 2.0 * juce::MathConstants<double>::pi)
                        ph -= 2.0 * juce::MathConstants<double>::pi;
                    block.setSample(0, s, v);
                    block.setSample(1, s, v);
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            binaural.processBlock(block);
            totalProcessUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            for (int ch = 0; ch < 2; ++ch)
                std::copy(block.getReadPointer(ch), block.getReadPointer(ch) + feedSize,
                          out[static_cast<size_t>(ch)].begin() + b * feedSize);
        }
        const int unitSkip = static_cast<int>(2.0f * fs);
        float unitPeak = 0.0f, unitMaxDelta = 0.0f;
        size_t unitMaxIdx = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            for (size_t i = static_cast<size_t>(unitSkip) + 1; i < out[static_cast<size_t>(ch)].size(); ++i)
            {
                unitPeak = std::max(unitPeak, std::abs(out[static_cast<size_t>(ch)][i]));
                const float d = std::abs(out[static_cast<size_t>(ch)][i] - out[static_cast<size_t>(ch)][i - 1]);
                if (d > unitMaxDelta)
                {
                    unitMaxDelta = d;
                    unitMaxIdx = i;
                }
            }
        }
        const double avgUs = (unitNumBlocks > 0) ? totalProcessUs / unitNumBlocks : 0.0;
        const double blockRealUs = 1.0e6 * (double)feedSize / fs;
        return juce::String::formatted("[PROBE] unit: maxDelta/peakAbs=%.4f (peak=%.4f, at sample %d, blockOffset %d) "
                                       "prepare=%d feed=%d avgProcess=%.1fus cpu=%.1f%%",
                                       unitPeak > 0 ? unitMaxDelta / unitPeak : 0.0f, unitPeak, (int)unitMaxIdx,
                                       (int)(unitMaxIdx % (size_t)blockSize), prepareSize, feedSize, avgUs,
                                       100.0 * avgUs / blockRealUs);
    }

    processor.prepareToPlay(fs, blockSize);
    configurePerTapProcessor(processor);
    setChoiceParam(processor, "SpatialPattern", 1);       // Orbit pattern enables motion
    setChoiceParam(processor, "SpatialChoreography", 1);  // Constellation
    setFloatParam(processor, "SpatialSpread", 100.0f);
    setFloatParam(processor, "SpatialRate", 0.35f);
    setFloatParam(processor, "SpatialDepth", 60.0f);

    // Diagnostic probes (not part of the gate): SPATIAL_ECHO_SMOOTH_PROBE=static
    // disables motion (isolates the static per-tap render); =linked switches to
    // Linked mode (isolates the shared global-binaural path under motion).
    if (const char* probe = std::getenv("SPATIAL_ECHO_SMOOTH_PROBE"))
    {
        if (std::strcmp(probe, "static") == 0)
            setChoiceParam(processor, "SpatialPattern", 0);  // Still (motion off)
        else if (std::strcmp(probe, "linked") == 0)
            setChoiceParam(processor, "HeadPositionMode", 0);
        else if (std::strcmp(probe, "nobinaural") == 0)
        {
            setChoiceParam(processor, "SpatialPattern", 0);  // Still (motion off)
            setBoolParam(processor, "SpatialEnabled", false);
        }
        else if (std::strncmp(probe, "linked-", 7) == 0)
        {
            // Stage-bisect probes: linked mode, motion off, one binaural
            // sub-stage disabled on voice 0.
            setChoiceParam(processor, "HeadPositionMode", 0);
            setChoiceParam(processor, "SpatialPattern", 0);  // Still (motion off)
            if (auto* v0 = processor.getBinauralProcessor())
            {
                if (std::strcmp(probe, "linked-noer") == 0)
                    v0->setDebugDisableEarlyReflections(true);
                else if (std::strcmp(probe, "linked-noair") == 0)
                    v0->setDebugDisableAirAbsorption(true);
                else if (std::strcmp(probe, "linked-nodvf") == 0)
                    v0->setDebugDisableNearFieldDvfForTest(true);
                else if (std::strcmp(probe, "linked-nosafety") == 0)
                    v0->setDebugDisableNearFieldSafetyForTest(true);
                else if (std::strcmp(probe, "linked-nodfe") == 0)
                    v0->setDebugDisableDiffuseEQ(true);
            }
        }
    }

    // Stimulus: a steady 220 Hz sine. Its own sample-to-sample delta is only
    // 2*pi*220/48000 ~= 0.029 of peak, so any position-update click stands out.
    // (White noise is useless here: adjacent noise samples legitimately jump
    // by up to ~2x peak, which is exactly what a click detector flags.)
    std::vector<std::vector<float>> buffer(2, std::vector<float>(numBlocks * blockSize, 0.0f));
    juce::MidiBuffer midi;
    double phase = 0.0;
    const double phaseInc = 2.0 * juce::MathConstants<double>::pi * sineFreqHz / fs;

    for (int b = 0; b < numBlocks; ++b)
    {
        juce::AudioBuffer<float> juceBuf(2, blockSize);
        for (int s = 0; s < blockSize; ++s)
        {
            const float v = stimulusAmp * static_cast<float>(std::sin(phase));
            phase += phaseInc;
            if (phase > 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;
            juceBuf.setSample(0, s, v);
            juceBuf.setSample(1, s, v);
        }

        processor.processBlock(juceBuf, midi);

        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = juceBuf.getReadPointer(ch);
            std::copy(src, src + blockSize, buffer[static_cast<size_t>(ch)].begin() + b * blockSize);
        }
    }

    // Diagnostic: raw float32 dump of both channels for offline analysis.
    if (const char* dumpPrefix = std::getenv("SPATIAL_ECHO_SMOOTH_DUMP"))
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::File f(juce::String(dumpPrefix) + "_ch" + juce::String(ch) + ".f32");
            f.deleteFile();
            juce::FileOutputStream os(f);
            if (os.openedOk())
                os.write(buffer[static_cast<size_t>(ch)].data(),
                         buffer[static_cast<size_t>(ch)].size() * sizeof(float));
        }
    }

    // Skip the first 2 s: parameter snap from defaults + smoother convergence.
    const int skipSamples = static_cast<int>(2.0f * fs);

    float peakAbs = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (size_t i = static_cast<size_t>(skipSamples); i < buffer[static_cast<size_t>(ch)].size(); ++i)
            peakAbs = std::max(peakAbs, std::abs(buffer[static_cast<size_t>(ch)][i]));

    if (peakAbs < 1e-6f)
        return "[FAIL] PerTapMotionSmoothness: Output is silent";

    float maxDelta = 0.0f;
    size_t maxDeltaIndex = 0;
    int maxDeltaChannel = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        for (size_t i = static_cast<size_t>(skipSamples) + 1; i < buffer[static_cast<size_t>(ch)].size(); ++i)
        {
            const float d = std::abs(buffer[static_cast<size_t>(ch)][i] - buffer[static_cast<size_t>(ch)][i - 1]);
            if (d > maxDelta)
            {
                maxDelta = d;
                maxDeltaIndex = i;
                maxDeltaChannel = ch;
            }
        }
    }

    // Gate: a smoothly-moving spatialized sine should never jump more than
    // 0.15x peak between adjacent samples (sine baseline is ~0.03x; a raw
    // block-boundary position jump lands well above this).
    const float ratio = maxDelta / peakAbs;
    constexpr float thresholdRatio = 0.15f;

    if (ratio <= thresholdRatio)
        return juce::String::formatted("[PASS] PerTapMotionSmoothness: maxDelta/peakAbs=%.4f (gate <= %.2f)", ratio,
                                       thresholdRatio);

    // Diagnostics: how many independent click events, and local waveform context.
    int clickEvents = 0;
    size_t lastClick = 0;
    juce::String firstClicks;
    for (size_t i = static_cast<size_t>(skipSamples) + 1; i < buffer[static_cast<size_t>(maxDeltaChannel)].size(); ++i)
    {
        const float d = std::abs(buffer[static_cast<size_t>(maxDeltaChannel)][i] -
                                 buffer[static_cast<size_t>(maxDeltaChannel)][i - 1]);
        if (d > thresholdRatio * peakAbs)
        {
            if (lastClick == 0 || i - lastClick > 100)
            {
                ++clickEvents;
                if (clickEvents <= 5)
                    firstClicks += juce::String((int)i) + " ";
            }
            lastClick = i;
        }
    }
    juce::String context;
    for (size_t i = maxDeltaIndex - 4;
         i <= maxDeltaIndex + 4 && i < buffer[static_cast<size_t>(maxDeltaChannel)].size(); ++i)
        context += juce::String(buffer[static_cast<size_t>(maxDeltaChannel)][i], 4) + " ";

    return juce::String::formatted(
        juce::CharPointer_UTF8(
            "[FAIL] PerTapMotionSmoothness: maxDelta/peakAbs=%.4f (gate <= %.2f) — audible click/zipper "
            "(ch=%d sample=%d blockOffset=%d events=%d firstAt=[%s] context=[%s])"),
        ratio, thresholdRatio, maxDeltaChannel, (int)maxDeltaIndex, (int)(maxDeltaIndex % (size_t)blockSize),
        clickEvents, firstClicks.toRawUTF8(), context.toRawUTF8());
}

// ============================================================================
// TapEnvelopeMeter: Per-tap envelope meter basic functionality test
// ============================================================================
juce::String runTapEnvelopeMeterTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;

    EchoesModel model;
    model.Reset(fs, 2, blockSize);

    // Enable per-head capture for tap envelope metering
    if (model.getTapeDelay())
        model.getTapeDelay()->enablePerHeadCapture(true);

    // Test 1: Meters should exist and be readable (not crash)
    float meter0 = model.getTapEnvelopeLevel(0);
    float meter1 = model.getTapEnvelopeLevel(1);
    float meter2 = model.getTapEnvelopeLevel(2);

    // Initially they should be zero
    if (meter0 != 0.0f || meter1 != 0.0f || meter2 != 0.0f)
    {
        return juce::String::formatted("[FAIL] TapEnvelopeMeter: Meters not zero at init (m0=%.4f, m1=%.4f, m2=%.4f)",
                                       meter0, meter1, meter2);
    }

    // Test 2: Out-of-range access should return 0 safely
    float invalidMeter_neg = model.getTapEnvelopeLevel(-1);
    float invalidMeter_3 = model.getTapEnvelopeLevel(3);
    float invalidMeter_999 = model.getTapEnvelopeLevel(999);

    if (invalidMeter_neg != 0.0f || invalidMeter_3 != 0.0f || invalidMeter_999 != 0.0f)
    {
        return juce::String::formatted("[FAIL] TapEnvelopeMeter: Out-of-range access returned non-zero "
                                       "(idx=-1:%.4f, idx=3:%.4f, idx=999:%.4f)",
                                       invalidMeter_neg, invalidMeter_3, invalidMeter_999);
    }

    // Test 3: a right-only burst must drive every enabled tap meter. This
    // catches left-only capture implementations that look valid on mono input.
    int heads[3] = {1, 1, 1};
    model.UpdateParameters(0.5f, 0.5f, 0.8f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f,
                           0, true, 45.0f, 90.0f, 135.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f,
                           0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true,
                           0,  // spatialRoutingMode (Linked)
                           1.0f, 0.0f, 0.0f, ReverbMacroParams{}, TapSpatialParams{});

    for (int b = 0; b < 48; ++b)
    {
        std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));

        // Send the impulse only on the right channel.
        if (b == 0)
            buffer[1][0] = 1.0f;

        model.ProcessBuffer(buffer, blockSize);
    }

    // After processing, tap meters should be readable
    float meterAfterProcess0 = model.getTapEnvelopeLevel(0);
    float meterAfterProcess1 = model.getTapEnvelopeLevel(1);
    float meterAfterProcess2 = model.getTapEnvelopeLevel(2);

    if (!std::isfinite(meterAfterProcess0) || !std::isfinite(meterAfterProcess1) || !std::isfinite(meterAfterProcess2))
    {
        return juce::String::formatted("[FAIL] TapEnvelopeMeter: Meters returned invalid values after processing "
                                       "(m0=%f, m1=%f, m2=%f)",
                                       meterAfterProcess0, meterAfterProcess1, meterAfterProcess2);
    }

    // Meters should be >= 0
    if (meterAfterProcess0 < 0.0f || meterAfterProcess1 < 0.0f || meterAfterProcess2 < 0.0f)
    {
        return juce::String::formatted("[FAIL] TapEnvelopeMeter: Meters returned negative values "
                                       "(m0=%.4f, m1=%.4f, m2=%.4f)",
                                       meterAfterProcess0, meterAfterProcess1, meterAfterProcess2);
    }

    if (meterAfterProcess0 <= 1.0e-5f || meterAfterProcess1 <= 1.0e-5f || meterAfterProcess2 <= 1.0e-5f)
    {
        return juce::String::formatted("[FAIL] TapEnvelopeMeter: right-only tap activity missing "
                                       "(m0=%.6f, m1=%.6f, m2=%.6f)",
                                       meterAfterProcess0, meterAfterProcess1, meterAfterProcess2);
    }

    return "[PASS] TapEnvelopeMeter";
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
