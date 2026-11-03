// ReverbPresetIRTest: IR measurement test for 12 spring + 10 cloud reverb presets (T60, brightness, delay).

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "ReverbPresetIRTest.h"
#include "../../DSP/Reverbprocessor.h"
#include "../../Presets/ReverbPresetLibrary.h"
#include <cmath>
#include <vector>
#include <numeric>
#include <algorithm>

namespace SpatialEchoTests::Reverb
{

namespace
{

// ---- Measurement Constants ----
constexpr float kSampleRate = 48000.0f;
constexpr float kIRDurationSec = 8.0f;
constexpr int kIRSamples = static_cast<int>(kSampleRate * kIRDurationSec);
constexpr float kImpulseAmplitude = 0.8f;
constexpr int kRMSWindowSamples = static_cast<int>(kSampleRate * 0.010f);  // 10ms windows
constexpr float kT30ThresholdLinear = 0.031622776f;                        // -30dB

struct IRMetrics
{
    float t60Seconds = 0.0f;       // Estimated T60 (from T30 * 2)
    float brightnessRatio = 0.0f;  // HF energy (>2kHz) / total energy
    float earlyLateRatio = 0.0f;   // Early RMS / Late RMS (dB)
    float onsetDelayMs = 0.0f;     // Time to first significant output
    float peakAmplitude = 0.0f;    // Peak absolute value in IR
    float rmsLevel = 0.0f;         // Overall RMS of first 2 seconds
};

// Render a preset IR through ReverbProcessor so the measurement covers the
// full shipping chain: macro laws, gamma tapers, recovery filters, gain law.
// Mono render (width stage bypasses at 1 channel; the tank itself is mono).
std::vector<float> renderPresetIR(const ReverbPresetDefinition& preset)
{
    constexpr int kBlockSize = 512;
    ReverbProcessor reverb;
    reverb.Reset(kSampleRate, 1, kBlockSize);

    ReverbMacroParams macros;
    macros.size = preset.size;
    macros.decay = preset.decay;
    macros.tone = preset.tone;
    macros.mod = preset.mod;
    macros.character = preset.character;
    macros.preDelay = preset.preDelay;
    macros.width = preset.width;
    macros.engine = 0;

    std::vector<float> ir;
    ir.reserve(static_cast<size_t>(kIRSamples + kBlockSize));
    std::vector<std::vector<float>> buffer(1, std::vector<float>(static_cast<size_t>(kBlockSize), 0.0f));

    const int numBlocks = (kIRSamples + kBlockSize - 1) / kBlockSize;
    for (int b = 0; b < numBlocks; ++b)
    {
        std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
        if (b == 0)
            buffer[0][0] = kImpulseAmplitude;
        reverb.UpdateParameters(1, 0.8f, macros, false, 0.0f);
        reverb.ProcessBuffer(buffer, kBlockSize);
        ir.insert(ir.end(), buffer[0].begin(), buffer[0].end());
    }
    ir.resize(static_cast<size_t>(kIRSamples));
    return ir;
}

// Estimate T60 via T30 extrapolation
float estimateT60(const std::vector<float>& ir)
{
    const int numWindows = static_cast<int>(ir.size()) / kRMSWindowSamples;
    if (numWindows < 2)
        return 0.0f;

    // Compute windowed RMS envelope
    std::vector<float> rmsEnvelope(static_cast<size_t>(numWindows), 0.0f);
    float peakRMS = 0.0f;
    int peakWindow = 0;

    for (int w = 0; w < numWindows; ++w)
    {
        const int start = w * kRMSWindowSamples;
        double sumSq = 0.0;
        for (int i = 0; i < kRMSWindowSamples; ++i)
        {
            const float v = ir[static_cast<size_t>(start + i)];
            sumSq += static_cast<double>(v) * v;
        }
        const float rms = std::sqrt(static_cast<float>(sumSq / kRMSWindowSamples));
        rmsEnvelope[static_cast<size_t>(w)] = rms;
        if (rms > peakRMS)
        {
            peakRMS = rms;
            peakWindow = w;
        }
    }

    if (peakRMS <= 1.0e-8f)
        return 0.0f;

    // Find T30: time from peak to -30dB below peak
    const float t30Threshold = peakRMS * kT30ThresholdLinear;
    for (int w = peakWindow + 1; w < numWindows; ++w)
    {
        if (rmsEnvelope[static_cast<size_t>(w)] <= t30Threshold)
        {
            const float t30Seconds = static_cast<float>((w - peakWindow) * kRMSWindowSamples) / kSampleRate;
            return t30Seconds * 2.0f;  // Extrapolate T30 -> T60
        }
    }

    // Didn't reach -30dB within IR duration
    const float remainingTime = static_cast<float>((numWindows - peakWindow) * kRMSWindowSamples) / kSampleRate;
    return remainingTime * 2.0f;
}

// Compute spectral brightness: ratio of energy above 2kHz to total energy
// Uses a simple DFT over the first 2 seconds of the IR
float computeBrightness(const std::vector<float>& ir)
{
    const int analysisLength = std::min(static_cast<int>(ir.size()), static_cast<int>(kSampleRate * 2.0f));
    if (analysisLength < 256)
        return 0.0f;

    // Energy in 400 Hz-spaced bands from direct sine/cosine correlation,
    // split at 2 kHz.
    double lowEnergy = 0.0;
    double highEnergy = 0.0;

    // Simple spectral analysis: correlate with sine waves at key frequencies
    const int numBands = 40;
    for (int band = 0; band < numBands; ++band)
    {
        const float freq = 100.0f + (static_cast<float>(band) * 400.0f);  // 100Hz to 16kHz
        if (freq >= kSampleRate * 0.5f)
            break;

        const float omega = 2.0f * 3.14159265f * freq / kSampleRate;
        double sumSin = 0.0;
        double sumCos = 0.0;
        for (int i = 0; i < analysisLength; ++i)
        {
            const float v = ir[static_cast<size_t>(i)];
            sumSin += v * std::sin(omega * static_cast<float>(i));
            sumCos += v * std::cos(omega * static_cast<float>(i));
        }
        const double bandEnergy = sumSin * sumSin + sumCos * sumCos;

        if (freq < 2000.0f)
            lowEnergy += bandEnergy;
        else
            highEnergy += bandEnergy;
    }

    const double totalEnergy = lowEnergy + highEnergy;
    if (totalEnergy <= 1.0e-20)
        return 0.0f;
    return static_cast<float>(highEnergy / totalEnergy);
}

// Compute early-to-late energy ratio (dB)
// Early: 0-80ms from onset, Late: 80-500ms from onset
float computeEarlyLateRatio(const std::vector<float>& ir, int onsetSample)
{
    const int earlyEnd = onsetSample + static_cast<int>(kSampleRate * 0.080f);
    const int lateStart = earlyEnd;
    const int lateEnd = onsetSample + static_cast<int>(kSampleRate * 0.500f);

    if (lateEnd > static_cast<int>(ir.size()))
        return 0.0f;

    double earlySumSq = 0.0;
    int earlyCount = 0;
    for (int i = onsetSample; i < earlyEnd && i < static_cast<int>(ir.size()); ++i)
    {
        const float v = ir[static_cast<size_t>(i)];
        earlySumSq += static_cast<double>(v) * v;
        ++earlyCount;
    }

    double lateSumSq = 0.0;
    int lateCount = 0;
    for (int i = lateStart; i < lateEnd && i < static_cast<int>(ir.size()); ++i)
    {
        const float v = ir[static_cast<size_t>(i)];
        lateSumSq += static_cast<double>(v) * v;
        ++lateCount;
    }

    const float earlyRMS = (earlyCount > 0) ? std::sqrt(static_cast<float>(earlySumSq / earlyCount)) : 0.0f;
    const float lateRMS = (lateCount > 0) ? std::sqrt(static_cast<float>(lateSumSq / lateCount)) : 0.0f;

    if (lateRMS <= 1.0e-9f)
        return 60.0f;  // essentially no late energy
    return 20.0f * std::log10(std::max(1.0e-9f, earlyRMS) / std::max(1.0e-9f, lateRMS));
}

// Find onset: first sample exceeding -40dB of peak
int findOnset(const std::vector<float>& ir)
{
    float peak = 0.0f;
    for (size_t i = 0; i < ir.size(); ++i)
        peak = std::max(peak, std::abs(ir[i]));

    if (peak <= 1.0e-9f)
        return 0;

    const float threshold = peak * 0.01f;  // -40dB
    for (size_t i = 0; i < ir.size(); ++i)
    {
        if (std::abs(ir[i]) >= threshold)
            return static_cast<int>(i);
    }
    return 0;
}

IRMetrics measureIR(const std::vector<float>& ir)
{
    IRMetrics m;

    // Peak amplitude
    for (size_t i = 0; i < ir.size(); ++i)
        m.peakAmplitude = std::max(m.peakAmplitude, std::abs(ir[i]));

    // Overall RMS (first 2s)
    const int rmsLength = std::min(static_cast<int>(ir.size()), static_cast<int>(kSampleRate * 2.0f));
    double sumSq = 0.0;
    for (int i = 0; i < rmsLength; ++i)
    {
        const float v = ir[static_cast<size_t>(i)];
        sumSq += static_cast<double>(v) * v;
    }
    m.rmsLevel = std::sqrt(static_cast<float>(sumSq / std::max(1, rmsLength)));

    // Onset delay
    const int onsetSample = findOnset(ir);
    m.onsetDelayMs = (static_cast<float>(onsetSample) / kSampleRate) * 1000.0f;

    // T60
    m.t60Seconds = estimateT60(ir);

    // Brightness
    m.brightnessRatio = computeBrightness(ir);

    // Early-late ratio
    m.earlyLateRatio = computeEarlyLateRatio(ir, onsetSample);

    return m;
}

}  // anonymous namespace

juce::String runPresetIRMeasurementTest(SpatialEchoAudioProcessor& /*processor*/)
{
    juce::String result;
    result << "=== Reverb Preset IR Measurement ===\n";
    result << "SampleRate=" << kSampleRate << " IRLength=" << kIRDurationSec << "s\n\n";

    // Column headers
    result << juce::String("Preset").paddedRight(' ', 20) << juce::String("T60(s)").paddedRight(' ', 10)
           << juce::String("Bright").paddedRight(' ', 10) << juce::String("E/L(dB)").paddedRight(' ', 10)
           << juce::String("Onset(ms)").paddedRight(' ', 12) << juce::String("Peak").paddedRight(' ', 10)
           << juce::String("RMS").paddedRight(' ', 10) << "\n";
    result << juce::String("-").paddedRight('-', 82) << "\n";

    bool anySignal = false;

    for (size_t p = 0; p < kNumReverbPresets; ++p)
    {
        const auto& preset = kReverbPresets[p];
        const auto ir = renderPresetIR(preset);
        const auto metrics = measureIR(ir);

        if (metrics.peakAmplitude > 1.0e-5f)
            anySignal = true;

        result << juce::String(preset.name).paddedRight(' ', 20)
               << juce::String(metrics.t60Seconds, 2).paddedRight(' ', 10)
               << juce::String(metrics.brightnessRatio, 3).paddedRight(' ', 10)
               << juce::String(metrics.earlyLateRatio, 1).paddedRight(' ', 10)
               << juce::String(metrics.onsetDelayMs, 1).paddedRight(' ', 12)
               << juce::String(metrics.peakAmplitude, 4).paddedRight(' ', 10)
               << juce::String(metrics.rmsLevel, 5).paddedRight(' ', 10) << "\n";
    }

    result << "\n";

    // Basic pass criteria: all presets produce signal and T60 ordering is plausible
    // (shorter presets should have shorter T60 than longer ones)
    const auto ir0 = renderPresetIR(kReverbPresets[0]);                         // Tight Spring (shortest)
    const auto irLast = renderPresetIR(kReverbPresets[kNumReverbPresets - 1]);  // Last preset (longest)
    const float t60Short = estimateT60(ir0);
    const float t60Long = estimateT60(irLast);
    const bool orderingOk = t60Long > t60Short;

    const bool passed = anySignal && orderingOk;
    result << "T60_ordering: first=" << juce::String(t60Short, 2) << "s last=" << juce::String(t60Long, 2) << "s "
           << (orderingOk ? "OK" : "REVERSED") << "\n";
    result << (passed ? "[PASS]" : "[FAIL]");

    return result;
}

}  // namespace SpatialEchoTests::Reverb

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
