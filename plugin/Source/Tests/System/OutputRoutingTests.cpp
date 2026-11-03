// OutputRoutingTests: Signal paths, mix and branch balance, output protection, Auto-Panic, block sizes, CPU
//   profile and processor lifecycle.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "SystemTests.h"
#include "../Common/TestUtilities.h"
#include "../../PluginProcessor.h"
#include "../../DSP/EchoesModel.h"
#include "EchoesModelTestAccess.h"
#include "../../DSP/TapeFlavors.h"
#include "../../DSP/OouraFFT.h"
#include "../../Presets/FactoryPresetLibrary.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

namespace SpatialEchoTests::System
{

juce::String runEqPathPlacementTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Test verifies EQ is applied in feedback path (affects echoes) not dry path.
    // Bass boost should affect echo tilt but not dry signal tilt.

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalSamples = static_cast<int>(fs);
    const int fftSize = 2048;

    std::vector<float> window(fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[static_cast<size_t>(i)] =
            0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) / (fftSize - 1)));

    auto renderResponse = [&](float bass, float mix) -> std::vector<float>
    {
        EchoesModel testModel;
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        ReverbMacroParams reverbMacros;
        testModel.UpdateParameters(bass, 0.5f, 0.5f, 0.5f, heads, 1, mix, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f,
                                   1.0f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f,
                                   0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f, 0.0f, reverbMacros);

        // Settle
        for (int w = 0; w < settleBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        // Capture response to impulse
        std::vector<float> samples;
        samples.reserve(static_cast<size_t>(totalSamples));

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);
        samples.insert(samples.end(), buf[0].begin(), buf[0].end());

        while (static_cast<int>(samples.size()) < totalSamples)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
            const int remaining = totalSamples - static_cast<int>(samples.size());
            const int copyCount = std::min(blkSize, remaining);
            samples.insert(samples.end(), buf[0].begin(), buf[0].begin() + copyCount);
        }

        return samples;
    };

    auto measureTilt = [&](float bass, float& dryTiltDb, float& echoTiltDb, int& dryPeakIdx, int& echoPeakIdx) -> bool
    {
        const std::vector<float> drySamples = renderResponse(bass, 0.0f);
        const std::vector<float> echoSamples = renderResponse(bass, 1.0f);

        // Find dry peak (first 10ms)
        const int drySearchEnd = std::min(static_cast<int>(drySamples.size()), static_cast<int>(0.01f * fs));
        dryPeakIdx = -1;
        float dryPeakVal = 0.0f;
        for (int i = 0; i < drySearchEnd; ++i)
        {
            const float a = std::abs(drySamples[static_cast<size_t>(i)]);
            if (a > dryPeakVal)
            {
                dryPeakVal = a;
                dryPeakIdx = i;
            }
        }

        // Find echo peak (after 50ms)
        const int echoSearchStart = std::min(static_cast<int>(echoSamples.size()), static_cast<int>(0.05f * fs));
        echoPeakIdx = -1;
        float echoPeakVal = 0.0f;
        for (int i = echoSearchStart; i < static_cast<int>(echoSamples.size()); ++i)
        {
            const float a = std::abs(echoSamples[static_cast<size_t>(i)]);
            if (a > echoPeakVal)
            {
                echoPeakVal = a;
                echoPeakIdx = i;
            }
        }

        if (dryPeakIdx < 0 || echoPeakIdx < 0)
            return false;

        // FFT analysis
        OouraFFT fft;
        fft.init(static_cast<size_t>(fftSize));
        std::vector<float> inDry(fftSize, 0.0f);
        std::vector<float> inEcho(fftSize, 0.0f);
        std::vector<std::complex<float>> specDry(fftSize / 2 + 1);
        std::vector<std::complex<float>> specEcho(fftSize / 2 + 1);

        auto fillWindow = [&](std::vector<float>& in, const std::vector<float>& source, int centerIndex)
        {
            for (int i = 0; i < fftSize; ++i)
            {
                const int idx = centerIndex - fftSize / 2 + i;
                const float sample =
                    (idx >= 0 && idx < static_cast<int>(source.size())) ? source[static_cast<size_t>(idx)] : 0.0f;
                in[static_cast<size_t>(i)] = sample * window[static_cast<size_t>(i)];
            }
        };

        fillWindow(inDry, drySamples, dryPeakIdx);
        fillWindow(inEcho, echoSamples, echoPeakIdx);

        fft.fft(inDry.data(), specDry.data());
        fft.fft(inEcho.data(), specEcho.data());

        auto magAt = [&](const std::vector<std::complex<float>>& spec, float freq) -> float
        {
            const int bin =
                std::min(static_cast<int>(spec.size()) - 1, static_cast<int>(std::round(freq * fftSize / fs)));
            return std::abs(spec[static_cast<size_t>(bin)]);
        };

        const float dry100 = magAt(specDry, 100.0f);
        const float dry1k = magAt(specDry, 1000.0f);
        const float echo100 = magAt(specEcho, 100.0f);
        const float echo1k = magAt(specEcho, 1000.0f);

        dryTiltDb = 20.0f * std::log10(std::max(1e-9f, dry100) / std::max(1e-9f, dry1k));
        echoTiltDb = 20.0f * std::log10(std::max(1e-9f, echo100) / std::max(1e-9f, echo1k));
        return true;
    };

    float dryTiltNeutral = 0.0f, echoTiltNeutral = 0.0f;
    float dryTiltBoost = 0.0f, echoTiltBoost = 0.0f;
    int dryIdxNeutral = -1, echoIdxNeutral = -1;
    int dryIdxBoost = -1, echoIdxBoost = -1;

    const bool okNeutral = measureTilt(0.5f, dryTiltNeutral, echoTiltNeutral, dryIdxNeutral, echoIdxNeutral);
    const bool okBoost = measureTilt(1.0f, dryTiltBoost, echoTiltBoost, dryIdxBoost, echoIdxBoost);

    const float dryDelta = dryTiltBoost - dryTiltNeutral;
    const float echoDelta = echoTiltBoost - echoTiltNeutral;

    // Pass: dry signal unaffected (<1dB), echo affected (>2dB bass boost)
    const bool passed = okNeutral && okBoost && (std::abs(dryDelta) <= 1.0f) && (echoDelta >= 2.0f);

    juce::String result;
    result << "EQ Path: dryDelta=" << juce::String(dryDelta, 1) << "dB echoDelta=" << juce::String(echoDelta, 1)
           << "dB (dry@" << dryIdxNeutral << "/" << dryIdxBoost << " echo@" << echoIdxNeutral << "/" << echoIdxBoost
           << " smp) " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runPreampPathCoverageTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blockSize = 512;
    const int settleBlocks = 16;
    const int totalSamples = static_cast<int>(fs);
    const float inputLow = 1.0f;
    const float inputHigh = 3.98107f;

    auto render = [&](float mix, int delayEnabled, int reverbEnabled, float inputGain, std::vector<float>& out)
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);

        int heads[3] = {delayEnabled ? 1 : 0, 0, 0};
        ReverbMacroParams reverbMacros;
        model.UpdateParameters(0.5f, 0.5f, 0.8f, 0.5f, heads, delayEnabled, mix, reverbEnabled, inputGain, 1.0f, false,
                               0.0f, 0.3f, 0.0f, 0.5f, 0.5f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f,
                               100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f,
                               0.0f, reverbMacros);

        std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));
        for (int i = 0; i < settleBlocks; ++i)
        {
            std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
            std::fill(buffer[1].begin(), buffer[1].end(), 0.0f);
            model.ProcessBuffer(buffer, blockSize);
        }

        out.assign(static_cast<size_t>(totalSamples), 0.0f);
        int writeIndex = 0;

        std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
        std::fill(buffer[1].begin(), buffer[1].end(), 0.0f);
        buffer[0][0] = 0.5f;
        buffer[1][0] = 0.5f;
        model.ProcessBuffer(buffer, blockSize);

        for (int i = 0; i < blockSize && writeIndex < totalSamples; ++i, ++writeIndex)
            out[(size_t)writeIndex] = buffer[0][static_cast<size_t>(i)];

        while (writeIndex < totalSamples)
        {
            std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
            std::fill(buffer[1].begin(), buffer[1].end(), 0.0f);
            model.ProcessBuffer(buffer, blockSize);
            const int remaining = totalSamples - writeIndex;
            const int copyCount = std::min(blockSize, remaining);
            for (int i = 0; i < copyCount; ++i)
                out[(size_t)writeIndex + (size_t)i] = buffer[0][static_cast<size_t>(i)];
            writeIndex += copyCount;
        }
    };

    auto peakInRange = [&](const std::vector<float>& samples, int start, int end) -> float
    {
        const int clampStart = std::max(0, start);
        const int clampEnd = std::min((int)samples.size(), end);
        float peak = 0.0f;
        for (int i = clampStart; i < clampEnd; ++i)
            peak = std::max(peak, std::abs(samples[(size_t)i]));
        return peak;
    };

    auto rmsInRange = [&](const std::vector<float>& samples, int start, int end) -> float
    {
        const int clampStart = std::max(0, start);
        const int clampEnd = std::min((int)samples.size(), end);
        if (clampEnd <= clampStart)
            return 0.0f;
        double sumSq = 0.0;
        for (int i = clampStart; i < clampEnd; ++i)
        {
            const float v = samples[(size_t)i];
            sumSq += static_cast<double>(v) * v;
        }
        const int count = clampEnd - clampStart;
        return (count > 0) ? static_cast<float>(std::sqrt(sumSq / count)) : 0.0f;
    };

    std::vector<float> dryLow, dryHigh;
    std::vector<float> echoLow, echoHigh;
    std::vector<float> reverbLow, reverbHigh;

    render(0.0f, 0, 0, inputLow, dryLow);
    render(0.0f, 0, 0, inputHigh, dryHigh);
    render(1.0f, 1, 0, inputLow, echoLow);
    render(1.0f, 1, 0, inputHigh, echoHigh);
    render(1.0f, 0, 1, inputLow, reverbLow);
    render(1.0f, 0, 1, inputHigh, reverbHigh);

    const int dryEnd = static_cast<int>(0.01f * fs);
    const int echoStart = static_cast<int>(0.05f * fs);
    const int reverbEnd = static_cast<int>(0.50f * fs);

    const float dryPeakLow = peakInRange(dryLow, 0, dryEnd);
    const float dryPeakHigh = peakInRange(dryHigh, 0, dryEnd);
    const float echoPeakLow = peakInRange(echoLow, echoStart, totalSamples);
    const float echoPeakHigh = peakInRange(echoHigh, echoStart, totalSamples);
    const float reverbRmsLow = rmsInRange(reverbLow, echoStart, reverbEnd);
    const float reverbRmsHigh = rmsInRange(reverbHigh, echoStart, reverbEnd);

    auto toDb = [](float v) -> float { return 20.0f * std::log10(std::max(1.0e-12f, v)); };
    const float dryDb = toDb(dryPeakHigh / std::max(1.0e-12f, dryPeakLow));
    const float echoDb = toDb(echoPeakHigh / std::max(1.0e-12f, echoPeakLow));
    const float reverbDb = toDb(reverbRmsHigh / std::max(1.0e-12f, reverbRmsLow));

    // Dry path should stay transparent (preamp is wet-only), while wet paths should track input gain.
    const bool dryOk = dryPeakLow > 1.0e-5f && std::abs(dryDb) <= 0.75f;
    const bool echoOk = echoPeakLow > 1.0e-5f && echoDb >= 3.0f;
    const bool reverbOk = reverbRmsLow > 1.0e-6f && reverbDb >= 3.0f;
    const bool passed = dryOk && echoOk && reverbOk;

    juce::String result;
    result << "PreampPathCoverage: dryDelta=" << juce::String(dryDb, 1) << "dB"
           << " echo=" << juce::String(echoDb, 1) << "dB"
           << " reverb=" << juce::String(reverbDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runToneStackIsolationTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blockSize = 512;
    const int settleBlocks = 8;
    const int totalSamples = 4096;
    const float freq = 1000.0f;
    const float amp = 0.1f;
    const float inputGain = 3.98107f;

    auto renderSine = [&](float bass, float treble, std::vector<float>& out)
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);

        int heads[3] = {0, 0, 0};
        ReverbMacroParams reverbMacros;
        model.UpdateParameters(bass, treble, 0.5f, 0.5f, heads, 0, 0.0f, 0, inputGain, 1.0f, false, 0.0f, 0.3f, 0.0f,
                               0.5f, 0.5f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f,
                               0.5f, 0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f, 0.0f, reverbMacros);

        std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));
        for (int i = 0; i < settleBlocks; ++i)
        {
            std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
            std::fill(buffer[1].begin(), buffer[1].end(), 0.0f);
            model.ProcessBuffer(buffer, blockSize);
        }

        out.assign(static_cast<size_t>(totalSamples), 0.0f);
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * freq / fs;
        int writeIndex = 0;
        while (writeIndex < totalSamples)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = amp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buffer[0][static_cast<size_t>(i)] = s;
                buffer[1][static_cast<size_t>(i)] = s;
            }

            model.ProcessBuffer(buffer, blockSize);

            const int remaining = totalSamples - writeIndex;
            const int copyCount = std::min(blockSize, remaining);
            for (int i = 0; i < copyCount; ++i)
                out[(size_t)writeIndex + (size_t)i] = buffer[0][static_cast<size_t>(i)];
            writeIndex += copyCount;
        }
    };

    std::vector<float> neutral;
    std::vector<float> extreme;
    renderSine(0.0f, 0.0f, neutral);
    renderSine(1.0f, 1.0f, extreme);

    double sumSqDiff = 0.0;
    double sumSqRef = 0.0;
    for (int i = 0; i < totalSamples; ++i)
    {
        const float diff = neutral[(size_t)i] - extreme[(size_t)i];
        sumSqDiff += static_cast<double>(diff) * diff;
        const float ref = neutral[(size_t)i];
        sumSqRef += static_cast<double>(ref) * ref;
    }

    const float diffRms = static_cast<float>(std::sqrt(sumSqDiff / totalSamples));
    const float refRms = static_cast<float>(std::sqrt(sumSqRef / totalSamples));
    const float diffDb = 20.0f * std::log10(std::max(1.0e-12f, diffRms / std::max(1.0e-12f, refRms)));
    const bool passed = (refRms > 1.0e-6f) && (diffDb <= -50.0f);

    juce::String result;
    result << "ToneStackIsolation: diff=" << juce::String(diffDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runMixSafetyTest(SpatialEchoAudioProcessor& processor)
{
    const float fs = 48000.0f;
    const int blockSize = 512;
    const float durationSec = 2.0f;
    const int totalBlocks = static_cast<int>(fs * durationSec) / blockSize;

    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };
    auto setChoiceParam = [&](const char* id, int index)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float norm = (maxIdx > 0) ? (float)clamped / (float)maxIdx : 0.0f;
            p->setValueNotifyingHost(norm);
        }
    };

    setFloatParam("InputLevel", 1.0f);
    setFloatParam("Intensity", 1.0f);
    setFloatParam("RepeatRate", 0.5f);
    setFloatParam("Mix", 1.0f);
    setFloatParam("Bass", 0.5f);
    setFloatParam("Treble", 0.5f);
    setBoolParam("MixLock", false);
    setBoolParam("EchoSend", true);
    setBoolParam("OutputProtection", true);
    setBoolParam("Power", true);
    setChoiceParam("DelaySetting", 6);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;

    float peak = 0.0f;
    bool finiteOk = true;
    for (int b = 0; b < totalBlocks; ++b)
    {
        buffer.clear();
        if (b == 0)
        {
            buffer.setSample(0, 0, 0.8f);
            buffer.setSample(1, 0, 0.8f);
        }

        processor.processBlock(buffer, midi);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float v = data[i];
                if (!std::isfinite(v))
                    finiteOk = false;
                peak = std::max(peak, std::abs(v));
            }
        }
    }

    const bool hasSignal = peak > 0.01f;
    const bool peakOk = peak <= 1.05f;

    // Regression check for dry contract:
    // With wet effectively silent, Mix follows linear crossfade (dry gain = 1 - Mix).
    const float mixProbe = 0.5f;
    setChoiceParam("DelaySetting", 0);  // Delay-only mode (no reverb energy in this block)
    setFloatParam("Intensity", 0.0f);   // Minimise any delayed wet output
    setFloatParam("Mix", mixProbe);
    setBoolParam("OutputProtection", false);

    juce::AudioBuffer<float> unityBuffer(2, blockSize);
    juce::MidiBuffer unityMidi;

    // Clear any earlier tape-state residue before measuring the dry contract.
    // Power=true alone does not clear buffers; the delay kill/clear path only
    // runs on an OFF->ON transition after the 20 ms kill fade completes.
    setBoolParam("Power", false);
    for (int i = 0; i < 3; ++i)
    {
        unityBuffer.clear();
        processor.processBlock(unityBuffer, unityMidi);
    }

    setBoolParam("Power", true);
    // Let the internal smoother settle at the probe value before measuring.
    unityBuffer.clear();
    processor.processBlock(unityBuffer, unityMidi);

    float inSumSq = 0.0f;
    float outSumSq = 0.0f;
    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * 440.0f / fs;
    float phase = 0.0f;
    for (int i = 0; i < blockSize; ++i)
    {
        const float sample = 0.2f * std::sin(phase);
        phase += phaseInc;
        unityBuffer.setSample(0, i, sample);
        unityBuffer.setSample(1, i, sample);
        inSumSq += sample * sample;
    }
    processor.processBlock(unityBuffer, unityMidi);
    for (int i = 0; i < blockSize; ++i)
    {
        const float sample = unityBuffer.getSample(0, i);
        outSumSq += sample * sample;
    }

    const float inRms = std::sqrt(inSumSq / static_cast<float>(blockSize));
    const float outRms = std::sqrt(outSumSq / static_cast<float>(blockSize));
    const float measuredDb = 20.0f * std::log10(std::max(1.0e-9f, outRms) / std::max(1.0e-9f, inRms));
    const float expectedDb = juce::Decibels::gainToDecibels(std::max(1.0e-9f, 1.0f - mixProbe));
    const bool linearWhenWetSilent = std::abs(measuredDb - expectedDb) <= 0.5f;

    const bool passed = finiteOk && hasSignal && peakOk && linearWhenWetSilent;

    juce::String result;
    result << "MixSafety: peak=" << juce::String(peak, 3) << " measuredDb=" << juce::String(measuredDb, 2)
           << " expectedDb=" << juce::String(expectedDb, 2) << " finite=" << (finiteOk ? "yes" : "no") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runActivePathMixMatrixTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);

    constexpr float fs = 48000.0f;
    constexpr int blockSize = 512;
    constexpr int settleBlocks = 24;
    constexpr int renderBlocks = 96;
    constexpr float impulse = 0.5f;
    const int totalSamples = renderBlocks * blockSize;

    struct MixCase
    {
        const char* name;
        std::array<int, 3> heads;
        int delayEnabled;
        int reverbEnabled;
    };

    struct Render
    {
        std::vector<float> left;
        std::vector<float> right;
        float peak = 0.0f;
        bool finite = true;
    };

    const std::array<MixCase, 3> cases = {
        {{"delay", {1, 0, 0}, 1, 0}, {"reverb", {0, 0, 0}, 0, 1}, {"dual", {1, 0, 0}, 1, 1}}};

    auto updateModel = [](EchoesModel& model, const MixCase& mixCase, float mix)
    {
        int heads[3] = {mixCase.heads[0], mixCase.heads[1], mixCase.heads[2]};
        ReverbMacroParams reverbMacros;
        reverbMacros.size = 0.70f;
        reverbMacros.decay = 0.60f;
        reverbMacros.tone = juce::jlimit(0.0f, 1.0f, 1.0f - 0.35f / 1.128f);
        reverbMacros.character = 0.5f;  // springBalance
        reverbMacros.mod = 0.0f;        // reverbModDepth
        reverbMacros.preDelay = 0.0f;
        reverbMacros.width = 0.5f;
        reverbMacros.engine = 0;  // Spring
        model.UpdateParameters(0.5f, 0.5f, 0.35f, 0.5f, heads, mixCase.delayEnabled, mix, mixCase.reverbEnabled, 0.75f,
                               1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f,
                               0,  // stereoMode
                               true, 80.0f, 160.0f, 240.0f, false, 0.0f, 0.0f, 1.0f, 0.0f, 150.0f, 2, 0.2f, 0.2f, 0.5f,
                               0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f, 0.0f, reverbMacros);
    };

    auto render = [&](const MixCase& mixCase, float mix) -> Render
    {
        EchoesModel model;
        model.Reset(fs, 2, blockSize);
        updateModel(model, mixCase, mix);

        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        for (int b = 0; b < settleBlocks; ++b)
        {
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);
            model.ProcessBuffer(buffer, blockSize);
        }

        Render out;
        out.left.reserve(static_cast<size_t>(totalSamples));
        out.right.reserve(static_cast<size_t>(totalSamples));

        for (int b = 0; b < renderBlocks; ++b)
        {
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);
            if (b == 0)
            {
                buffer[0][0] = impulse;
                buffer[1][0] = impulse;
            }

            model.ProcessBuffer(buffer, blockSize);

            for (int i = 0; i < blockSize; ++i)
            {
                const float left = buffer[0][static_cast<size_t>(i)];
                const float right = buffer[1][static_cast<size_t>(i)];
                out.left.push_back(left);
                out.right.push_back(right);
                out.peak = std::max(out.peak, std::max(std::abs(left), std::abs(right)));
                if (!std::isfinite(left) || !std::isfinite(right))
                    out.finite = false;
            }
        }

        return out;
    };

    auto maxDryIdentityError = [&](const Render& dry) -> float
    {
        float maxErr = 0.0f;
        for (size_t i = 0; i < dry.left.size(); ++i)
        {
            const float expected = (i == 0) ? impulse : 0.0f;
            maxErr = std::max(maxErr, std::abs(dry.left[i] - expected));
            maxErr = std::max(maxErr, std::abs(dry.right[i] - expected));
        }
        return maxErr;
    };

    auto maxHalfMixError = [](const Render& dry, const Render& half, const Render& wet) -> float
    {
        float maxErr = 0.0f;
        const size_t count = std::min(dry.left.size(), std::min(half.left.size(), wet.left.size()));
        for (size_t i = 0; i < count; ++i)
        {
            const float expectedLeft = 0.5f * (dry.left[i] + wet.left[i]);
            const float expectedRight = 0.5f * (dry.right[i] + wet.right[i]);
            maxErr = std::max(maxErr, std::abs(half.left[i] - expectedLeft));
            maxErr = std::max(maxErr, std::abs(half.right[i] - expectedRight));
        }
        return maxErr;
    };

    auto peakInRange = [](const Render& rendered, int start, int end) -> float
    {
        const int s0 = std::max(0, start);
        const int s1 = std::min(static_cast<int>(rendered.left.size()), end);
        float peak = 0.0f;
        for (int i = s0; i < s1; ++i)
        {
            peak = std::max(peak, std::abs(rendered.left[static_cast<size_t>(i)]));
            peak = std::max(peak, std::abs(rendered.right[static_cast<size_t>(i)]));
        }
        return peak;
    };

    const int wetStart = static_cast<int>(0.010f * fs);
    const int wetEnd = static_cast<int>(0.500f * fs);
    const float dryTolerance = 1.0e-5f;
    const float halfMixTolerance = 1.0e-4f;
    const float wetMinPeak = 1.0e-5f;
    bool passed = true;
    juce::String details;

    for (const auto& mixCase : cases)
    {
        const Render dry = render(mixCase, 0.0f);
        const Render half = render(mixCase, 0.5f);
        const Render wet = render(mixCase, 1.0f);

        const float dryErr = maxDryIdentityError(dry);
        const float halfErr = maxHalfMixError(dry, half, wet);
        const float wetPeak = peakInRange(wet, wetStart, wetEnd);
        const float wetPeakDb = juce::Decibels::gainToDecibels(std::max(1.0e-9f, wetPeak));
        const bool finiteOk = dry.finite && half.finite && wet.finite;
        const bool dryOk = dryErr <= dryTolerance;
        const bool halfOk = halfErr <= halfMixTolerance;
        const bool wetOk = wetPeak >= wetMinPeak;
        const bool caseOk = finiteOk && dryOk && halfOk && wetOk;
        passed = passed && caseOk;

        details << " " << mixCase.name << "(dryErr=" << juce::String(dryErr, 6)
                << ",halfErr=" << juce::String(halfErr, 6) << ",wet=" << juce::String(wetPeakDb, 1) << "dB"
                << (caseOk ? "" : "!") << ")";
    }

    juce::String result;
    result << "ActivePathMixMatrix:" << details << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runBranchTiltModeSwitchContinuityTest(SpatialEchoAudioProcessor& processor)
{
    constexpr std::array<double, 2> sampleRates = {44100.0, 48000.0};
    constexpr int blockSize = 256;
    constexpr int warmBlocks = 320;
    constexpr int baselineBlocks = 12;
    constexpr int transitionBlocks = 4;
    constexpr float sineFrequency = 220.0f;
    constexpr float sineAmplitude = 0.2f;

    auto& vts = processor.getValueTreeState();
    // Setup helpers fail loudly on stale/mistyped parameter IDs so a silent
    // no-op can never mask drifted test preconditions.
    auto setFloatParam = [&](const char* id, float value)
    {
        auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id));
        jassert(p != nullptr);
        if (p != nullptr)
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id));
        jassert(p != nullptr);
        if (p != nullptr)
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };
    auto setChoiceParam = [&](const char* id, int index)
    {
        auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id));
        jassert(p != nullptr);
        if (p != nullptr)
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float normalized = maxIdx > 0 ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
            p->setValueNotifyingHost(normalized);
        }
    };

    struct RateResult
    {
        float baseline = 0.0f;
        float toDual = 0.0f;
        float toReverbOnly = 0.0f;
        float limit = 0.0f;
        bool finite = true;
        bool passed = false;
    };

    auto runAtRate = [&](double sampleRate) -> RateResult
    {
        processor.prepareToPlay(sampleRate, blockSize);
        setChoiceParam("DelaySetting", 11);  // Reverb Only
        setChoiceParam("ReverbRouting", 0);  // Parallel
        setFloatParam("Mix", 1.0f);
        setFloatParam("InputLevel", 1.0f);
        setFloatParam("Intensity", 0.35f);
        setFloatParam("RepeatRate", 0.5f);
        setFloatParam("Bass", 0.5f);
        setFloatParam("Treble", 0.5f);
        setFloatParam("ReverbSize", 0.65f);
        setFloatParam("ReverbDecay", 0.8f);
        setFloatParam("ReverbPreDelay", 0.0f);
        setFloatParam("ReverbDuckAmount", 0.0f);
        setBoolParam("MixLock", false);
        setBoolParam("EchoSend", true);
        // No host noise parameter exists (per-flavor internal since the schema
        // cull); pin CLEAN so tape noise is deterministically off.
        setChoiceParam("TapeFlavor", 0);
        setBoolParam("SpatialEnabled", false);
        setBoolParam("OutputProtection", false);
        setBoolParam("Power", true);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        float phase = 0.0f;
        float lastOutput = 0.0f;
        bool hasLastOutput = false;
        RateResult rate;
        const float phaseIncrement =
            2.0f * juce::MathConstants<float>::pi * sineFrequency / static_cast<float>(sampleRate);

        auto processBlocks = [&](int blocks) -> float
        {
            float maxAdjacentDelta = 0.0f;
            for (int block = 0; block < blocks; ++block)
            {
                for (int sampleIndex = 0; sampleIndex < blockSize; ++sampleIndex)
                {
                    const float sample = sineAmplitude * std::sin(phase);
                    phase += phaseIncrement;
                    if (phase >= 2.0f * juce::MathConstants<float>::pi)
                        phase -= 2.0f * juce::MathConstants<float>::pi;
                    buffer.setSample(0, sampleIndex, sample);
                    buffer.setSample(1, sampleIndex, sample);
                }

                processor.processBlock(buffer, midi);
                for (int sampleIndex = 0; sampleIndex < blockSize; ++sampleIndex)
                {
                    const float output = buffer.getSample(0, sampleIndex);
                    rate.finite = rate.finite && std::isfinite(output);
                    if (hasLastOutput)
                        maxAdjacentDelta = std::max(maxAdjacentDelta, std::abs(output - lastOutput));
                    lastOutput = output;
                    hasLastOutput = true;
                }
            }
            return maxAdjacentDelta;
        };

        processBlocks(warmBlocks);
        rate.baseline = processBlocks(baselineBlocks);
        rate.limit = rate.baseline * 3.0f;

        setChoiceParam("DelaySetting", 10);  // All Heads Echo+Verb
        rate.toDual = processBlocks(transitionBlocks);

        setChoiceParam("DelaySetting", 11);  // Reverb Only
        rate.toReverbOnly = processBlocks(transitionBlocks);

        rate.passed = rate.finite && rate.toDual <= rate.limit && rate.toReverbOnly <= rate.limit;
        return rate;
    };

    const RateResult at441 = runAtRate(sampleRates[0]);
    const RateResult at480 = runAtRate(sampleRates[1]);
    const bool passed = at441.passed && at480.passed;

    juce::String result;
    result << "BranchTiltModeSwitchContinuity:"
           << " 441{base=" << juce::String(at441.baseline, 6) << ",toDual=" << juce::String(at441.toDual, 6)
           << ",toRev=" << juce::String(at441.toReverbOnly, 6) << ",limit=" << juce::String(at441.limit, 6) << "}"
           << " 480{base=" << juce::String(at480.baseline, 6) << ",toDual=" << juce::String(at480.toDual, 6)
           << ",toRev=" << juce::String(at480.toReverbOnly, 6) << ",limit=" << juce::String(at480.limit, 6) << "} "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runDualPathBalanceDefaultTest(SpatialEchoAudioProcessor& processor)
{
    const float fs = 48000.0f;
    const int blockSize = 512;
    const int totalBlocks = 56;
    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };
    auto setChoiceParam = [&](const char* id, int index)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
            p->setValueNotifyingHost(norm);
        }
    };

    struct Render
    {
        std::vector<float> samples;
        float echoMeter = 0.0f;
        float reverbMeter = 0.0f;
        bool finite = true;
    };

    auto renderCapture = [&](int delaySetting, float intensity, float repeatRate, bool clickTrain,
                             int renderBlocks) -> Render
    {
        processor.prepareToPlay(fs, blockSize);
        setChoiceParam("DelaySetting", delaySetting);
        setChoiceParam("ReverbRouting", 0);  // Parallel
        setFloatParam("Mix", 1.0f);
        setFloatParam("InputLevel", 1.0f);
        setFloatParam("Intensity", intensity);
        setFloatParam("RepeatRate", repeatRate);
        setFloatParam("Bass", 0.5f);
        setFloatParam("Treble", 0.5f);
        setBoolParam("MixLock", false);
        setBoolParam("EchoSend", true);
        setBoolParam("OutputProtection", true);
        setBoolParam("Power", true);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        constexpr float kImpulse = 0.8f;
        const int totalSamples = renderBlocks * blockSize;
        Render render;
        render.samples.resize(static_cast<size_t>(totalSamples), 0.0f);
        int sampleIndex = 0;
        const int clickInterval = static_cast<int>(0.25f * fs);

        for (int b = 0; b < renderBlocks; ++b)
        {
            buffer.clear();
            if (!clickTrain && b == 0)
            {
                buffer.setSample(0, 0, kImpulse);
                buffer.setSample(1, 0, kImpulse);
            }
            else if (clickTrain)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    const int globalSample = b * blockSize + i;
                    if (globalSample % clickInterval == 0)
                    {
                        buffer.setSample(0, i, kImpulse);
                        buffer.setSample(1, i, kImpulse);
                    }
                }
            }

            processor.processBlock(buffer, midi);

            for (int i = 0; i < blockSize; ++i, ++sampleIndex)
            {
                if (sampleIndex >= 0 && sampleIndex < totalSamples)
                {
                    const float sample = buffer.getSample(0, i);
                    render.finite = render.finite && std::isfinite(sample);
                    render.samples[static_cast<size_t>(sampleIndex)] = sample;
                }
            }
        }

        render.echoMeter = processor.getEchoBranchMeterLevel();
        render.reverbMeter = processor.getReverbBranchMeterLevel();
        return render;
    };

    auto rmsInRange = [](const std::vector<float>& samples, int start, int end) -> float
    {
        const int s0 = std::max(0, start);
        const int s1 = std::min(static_cast<int>(samples.size()), end);
        if (s1 <= s0)
            return 0.0f;
        double sumSq = 0.0;
        for (int i = s0; i < s1; ++i)
        {
            const float v = samples[static_cast<size_t>(i)];
            sumSq += static_cast<double>(v) * v;
        }
        return static_cast<float>(std::sqrt(sumSq / static_cast<double>(s1 - s0)));
    };

    auto peakInRange = [](const std::vector<float>& samples, int start, int end) -> float
    {
        const int s0 = std::max(0, start);
        const int s1 = std::min(static_cast<int>(samples.size()), end);
        float peak = 0.0f;
        for (int i = s0; i < s1; ++i)
            peak = std::max(peak, std::abs(samples[static_cast<size_t>(i)]));
        return peak;
    };

    auto ratioToDb = [](float numerator, float denominator) -> float
    { return 20.0f * std::log10(std::max(1.0e-9f, numerator) / std::max(1.0e-9f, denominator)); };

    const int echoSearchStart = static_cast<int>(0.07f * fs);
    const int echoSearchEnd = static_cast<int>(0.25f * fs);

    const Render defaultRender = renderCapture(4, 0.4f, 0.5f, false, totalBlocks);
    const float defaultDeltaDb = ratioToDb(defaultRender.reverbMeter, defaultRender.echoMeter);

    // User-reported stress case: high feedback, slower repeat rate in parallel mode.
    const Render userRender = renderCapture(4, 0.82f, 0.2f, false, totalBlocks);
    const float userDeltaDb = ratioToDb(userRender.reverbMeter, userRender.echoMeter);

    // Click-train stress metric: closer to real use (ongoing source + accumulating reverb).
    constexpr int kStressBlocks = 320;  // ~3.4s
    const Render stressRender = renderCapture(4, 0.82f, 0.2f, true, kStressBlocks);

    const Render delayOnlyRender = renderCapture(0, 0.4f, 0.5f, false, totalBlocks);
    const Render reverbOnlyRender = renderCapture(11, 0.4f, 0.5f, false, totalBlocks);
    const float delayOnlyInactiveDb = juce::Decibels::gainToDecibels(delayOnlyRender.reverbMeter, -160.0f);
    const float reverbOnlyInactiveDb = juce::Decibels::gainToDecibels(reverbOnlyRender.echoMeter, -160.0f);

    EchoesModel balanceProbe;
    balanceProbe.Reset(fs, 2, blockSize);
    int probeHeads[3] = {1, 0, 0};
    balanceProbe.UpdateParameters(0.5f, 0.5f, 0.4f, 0.5f, probeHeads, 1, 1.0f, 1, 1.0f, 1.0f);
    std::vector<std::vector<float>> probeBuffer(2, std::vector<float>(blockSize, 0.0f));
    balanceProbe.ProcessBuffer(probeBuffer, blockSize);
    float dualEchoGain = 0.0f;
    float dualReverbGain = 0.0f;
    const bool dualGainsAvailable =
        EchoesModelTestAccess::getLastBranchBalanceGains(balanceProbe, dualEchoGain, dualReverbGain);
    const float dualGainRatioDb = ratioToDb(dualEchoGain, dualReverbGain);
    const float expectedDualGainRatioDb =
        ratioToDb(std::cos(juce::MathConstants<float>::pi * 0.25f), std::sin(juce::MathConstants<float>::pi * 0.25f));

    const int stressTotalSamples = static_cast<int>(stressRender.samples.size());
    const int clickInterval = static_cast<int>(0.25f * fs);
    double sumStressPeak = 0.0;
    double sumStressRms = 0.0;
    int stressCount = 0;

    for (int clickSample = 0; clickSample < stressTotalSamples; clickSample += clickInterval)
    {
        const int winStart = clickSample + echoSearchStart;
        const int winEnd = clickSample + echoSearchEnd;
        if (winEnd > stressTotalSamples)
            break;
        sumStressPeak += peakInRange(stressRender.samples, winStart, winEnd);
        sumStressRms += rmsInRange(stressRender.samples, winStart, winEnd);
        ++stressCount;
    }

    const float avgStressPeak = (stressCount > 0) ? static_cast<float>(sumStressPeak / stressCount) : 0.0f;
    const float avgStressRms = (stressCount > 0) ? static_cast<float>(sumStressRms / stressCount) : 0.0f;

    const bool finiteOk = defaultRender.finite && userRender.finite && stressRender.finite && delayOnlyRender.finite &&
                          reverbOnlyRender.finite && std::isfinite(defaultDeltaDb) && std::isfinite(userDeltaDb) &&
                          std::isfinite(dualGainRatioDb);
    const bool signalOk = defaultRender.echoMeter > 1.0e-6f && defaultRender.reverbMeter > 1.0e-6f &&
                          userRender.echoMeter > 1.0e-6f && userRender.reverbMeter > 1.0e-6f &&
                          stressRender.echoMeter > 1.0e-6f && stressRender.reverbMeter > 1.0e-6f &&
                          avgStressPeak > 1.0e-6f && avgStressRms > 1.0e-6f;
    const bool defaultOk = defaultDeltaDb <= 2.5f;
    const bool userOk = userDeltaDb <= 3.0f;
    const bool dualGainOk = dualGainsAvailable && std::abs(dualGainRatioDb - expectedDualGainRatioDb) <= 0.2f;
    const bool inactiveMetersOk = delayOnlyRender.echoMeter > 1.0e-6f && reverbOnlyRender.reverbMeter > 1.0e-6f &&
                                  delayOnlyInactiveDb <= -60.0f && reverbOnlyInactiveDb <= -60.0f;
    const bool passed = finiteOk && signalOk && defaultOk && userOk && dualGainOk && inactiveMetersOk;

    juce::String result;
    result << "DualPathBalanceDefault: defaultDelta=" << juce::String(defaultDeltaDb, 2)
           << "dB userDelta=" << juce::String(userDeltaDb, 2) << "dB stressPeak=" << juce::String(avgStressPeak, 4)
           << " stressRms=" << juce::String(avgStressRms, 4) << " echoMeter=" << juce::String(stressRender.echoMeter, 6)
           << " reverbMeter=" << juce::String(stressRender.reverbMeter, 6)
           << " dualGainRatio=" << juce::String(dualGainRatioDb, 3) << "dB"
           << " delayInactive=" << juce::String(delayOnlyInactiveDb, 1) << "dBFS"
           << " reverbInactive=" << juce::String(reverbOnlyInactiveDb, 1) << "dBFS"
           << " (max 2.5/3.0dB, dual +/-0.2dB, inactive <=-60dBFS) " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runBlockSizeExtremesTest(SpatialEchoAudioProcessor& processor)
{
    const double fs = 48000.0;
    const std::array<int, 4> blockSizes = {1, 16, 4096, 16384};

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };
    auto setChoiceParam = [&](const char* id, int index)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
        {
            const int maxIdx = std::max(0, p->choices.size() - 1);
            const int clamped = std::max(0, std::min(index, maxIdx));
            const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
            p->setValueNotifyingHost(norm);
        }
    };

    setFloatParam("InputLevel", 1.0f);
    setFloatParam("Intensity", 0.85f);
    setFloatParam("RepeatRate", 0.5f);
    setFloatParam("Mix", 1.0f);
    setFloatParam("Bass", 0.5f);
    setFloatParam("Treble", 0.5f);
    setBoolParam("MixLock", false);
    setBoolParam("EchoSend", true);
    setBoolParam("OutputProtection", true);
    // Noise-on stress state via the WORN flavor
    setChoiceParam("TapeFlavor", static_cast<int>(TapeFlavor::Worn));
    setBoolParam("Power", true);
    setChoiceParam("DelaySetting", 6);

    bool finiteAll = true;
    bool signalAll = true;
    juce::String details;

    for (int blockSize : blockSizes)
    {
        processor.prepareToPlay(fs, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;

        const int totalBlocks = std::max(1, static_cast<int>(std::ceil((fs * 0.75) / static_cast<double>(blockSize))));
        float peak = 0.0f;
        bool finiteOk = true;

        for (int b = 0; b < totalBlocks; ++b)
        {
            buffer.clear();
            if (b == 0)
            {
                buffer.setSample(0, 0, 0.8f);
                buffer.setSample(1, 0, 0.8f);
            }

            processor.processBlock(buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* data = buffer.getReadPointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float v = data[i];
                    if (!std::isfinite(v))
                        finiteOk = false;
                    peak = std::max(peak, std::abs(v));
                }
            }
        }

        const bool hasSignal = peak > 1.0e-4f;
        finiteAll = finiteAll && finiteOk;
        signalAll = signalAll && hasSignal;

        details << " " << blockSize << ":" << juce::String(peak, 3) << (finiteOk ? "" : "!finite")
                << (hasSignal ? "" : "!signal");
    }

    processor.releaseResources();

    const bool passed = finiteAll && signalAll;
    juce::String result;
    result << "BlockSizeExtremes:" << details << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runCpuStressProfileTest(SpatialEchoAudioProcessor& processor)
{
    struct ScopedHighPriority
    {
        ScopedHighPriority() { juce::Process::setPriority(juce::Process::HighPriority); }
        ~ScopedHighPriority() { juce::Process::setPriority(juce::Process::NormalPriority); }
    } scopedHighPriority;

    struct CpuProfileEntry
    {
        juce::String caseName;
        double sampleRate = 0.0;
        int blockSize = 0;
        float cpuPct = 0.0f;
        float cpuMinPct = 0.0f;
        float cpuMaxPct = 0.0f;
        float peak = 0.0f;
        bool finiteOk = true;
        EchoesModel::StageTimingSnapshot stageTiming{};
        bool hasStageTiming = false;
    };

    struct CpuCaseConfig
    {
        juce::String name;
        int delaySettingIndex = 0;
        bool binauralOn = true;
        bool noiseOn = true;
        int tapPositionMode = 0;       // 0 = Linked, 1 = Independent
        int motionTapMode = 0;         // 0 = Unison, 1 = Constellation, 2 = Cascade
        float motionTapSpread = 0.0f;  // 0.0 to 1.0 (100% max)
        int reverbEngine = 0;          // 0 = Spring, 1 = Cloud
    };

    struct CpuCaseSummary
    {
        juce::String name;
        float maxCpu = 0.0f;
        double maxFs = 0.0;
        int maxBlock = 0;
        float maxPeak = 0.0f;
        bool finiteAll = true;
        int overBudget = 0;
        int unstable = 0;
        std::vector<CpuProfileEntry> entries;
    };

    auto& vts = processor.getValueTreeState();
    juce::StringArray configurationErrors;
    auto setFloatParam = [&](const char* id, float normalized)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id));
        if (parameter == nullptr || !std::isfinite(normalized) || normalized < 0.0f || normalized > 1.0f)
        {
            configurationErrors.add(juce::String(id) + ": invalid float ID/type or normalized value");
            return false;
        }
        parameter->setValueNotifyingHost(normalized);
        return true;
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id));
        if (parameter == nullptr)
        {
            configurationErrors.add(juce::String(id) + ": invalid bool ID/type");
            return false;
        }
        parameter->setValueNotifyingHost(value ? 1.0f : 0.0f);
        return true;
    };
    auto setChoiceParam = [&](const char* id, int index)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id));
        if (parameter == nullptr || index < 0 || index >= parameter->choices.size())
        {
            configurationErrors.add(juce::String(id) + ": invalid choice ID/type or index");
            return false;
        }
        parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(index)));
        return true;
    };

    // Qualify the setters before trusting a stress measurement. Invalid IDs,
    // parameter types, and plain values passed as normalized must fail closed.
    if (setFloatParam("__missing_cpu_parameter__", 0.5f) || setFloatParam("SpatialDepth", 50.0f) ||
        setBoolParam("Mix", true) || setChoiceParam("ReverbEngine", 99) || configurationErrors.size() != 4)
        return "[ERROR] CpuStressProfile configuration validation self-test failed";
    configurationErrors.clear();

    auto applyStressParams = [&](const CpuCaseConfig& cfg)
    {
        setFloatParam("InputLevel", 1.0f);
        setFloatParam("Bass", 0.5f);
        setFloatParam("Treble", 0.5f);

        setChoiceParam("DelaySetting", cfg.delaySettingIndex);
        setFloatParam("RepeatRate", 0.5f);
        setFloatParam("Intensity", 1.0f);
        setBoolParam("TempoSync", false);
        setBoolParam("EchoSend", true);
        setBoolParam("Power", true);
        setBoolParam("PingPong", true);

        setChoiceParam("ReverbEngine", cfg.reverbEngine);
        // High reverb/feedback load in the macro schema
        // (tone 0 = maximum damping).
        setFloatParam("ReverbSize", 1.0f);
        setFloatParam("ReverbDecay", 1.0f);
        setFloatParam("ReverbTone", 0.0f);
        setFloatParam("ReverbMod", 1.0f);
        setFloatParam("ReverbCharacter", 1.0f);
        setFloatParam("ReverbPreDelay", 1.0f);
        setFloatParam("ReverbWidth", 1.0f);
        setChoiceParam("ReverbRouting", 0);

        setFloatParam("Mix", 1.0f);
        setBoolParam("MixLock", false);
        setBoolParam("OutputProtection", true);

        // Per-scenario noise via flavor: WORN = hiss/wear on, CLEAN = minimal character (old wf=0 baseline)
        setChoiceParam("TapeFlavor", static_cast<int>(cfg.noiseOn ? TapeFlavor::Worn : TapeFlavor::Clean));
        setFloatParam("TapeDrive", 1.0f);
        setBoolParam("TapeNoiseEnabled", cfg.noiseOn);

        setBoolParam("SpatialEnabled", cfg.binauralOn);
        setFloatParam("SpatialAzimuth", 0.5f);
        setFloatParam("SpatialElevation", 0.5f);
        setFloatParam("SpatialDistance", 1.0f);
        setFloatParam("SpatialMix", cfg.binauralOn ? 1.0f : 0.0f);
        setFloatParam("SpatialCrossover", 0.0f);
        setChoiceParam("SpatialQuality", 2);
        setFloatParam("SpatialSourceWidth", 1.0f);

        // Apply per-tap spatial settings. Motion must actually be ENABLED with
        // nonzero depth for the per-tap constellation path to run.
        setChoiceParam("HeadPositionMode", cfg.tapPositionMode);
        if (cfg.tapPositionMode == 1 && cfg.motionTapSpread > 0.0f)
        {
            // Independent + moving taps (worst case: continuous HRIR churn).
            setChoiceParam("SpatialChoreography", cfg.motionTapMode);
            setFloatParam("SpatialSpread", cfg.motionTapSpread);
            setChoiceParam("SpatialPattern", 1);  // Orbit (non-Still enables motion)
            setFloatParam("SpatialRate", 0.5f);   // normalized (mid-range rate)
            setFloatParam("SpatialDepth", 0.5f);  // normalized: 50% depth
        }
        else
        {
            setChoiceParam("SpatialPattern", 0);  // Still (motion off)
        }
        if (cfg.tapPositionMode == 1)
        {
            // Spread the static tap positions so voices render distinct HRIRs.
            setFloatParam("Head1Azimuth", 0.25f);  // normalized: -90 deg
            setFloatParam("Head2Azimuth", 0.75f);  // normalized: +90 deg
            setFloatParam("Head3Azimuth", 0.5f);   // normalized: 0 deg
        }
    };

    // Focus on the failing region (small blocks, higher rates) to keep runtime bounded.
    const std::array<double, 2> sampleRates = {48000.0, 96000.0};
    const std::array<int, 2> blockSizes = {64, 256};
    const float durationSec = 0.75f;
    const int settleBlocks = 3;
    constexpr int kStressRepeats = 3;
    constexpr float kCpuVarianceBudgetPct = 25.0f;

    // DelaySetting indexes:
    // - 10: all heads, echo+reverb enabled
    // - 11: reverb only (delay disabled by mode table)
    // - 3 : heads 2+3, reverb disabled
    // - 8 : heads 2+3, reverb enabled
    // TapPositionMode: 0 = Linked, 1 = Independent
    // MotionTapMode: 0 = Unison, 1 = Constellation, 2 = Cascade
    std::vector<CpuCaseConfig> cases = {
        {"full10", 10, true, true, 0, 0, 0.0f},
        {"full10_noBin", 10, false, true, 0, 0, 0.0f},
        {"full10_noNoise", 10, true, false, 0, 0, 0.0f},
        {"revOnly11", 11, true, true, 0, 0, 0.0f},
        {"revOnly11_noBin", 11, false, true, 0, 0, 0.0f},
        {"delayOnly3", 3, true, true, 0, 0, 0.0f},
        {"delayOnly3_noBin", 3, false, true, 0, 0, 0.0f},
        {"delayReverb8", 8, true, true, 0, 0, 0.0f},
        {"delayReverb8_noBin", 8, false, true, 0, 0, 0.0f},
        // Independent per-tap positioning (3 binaural voices).
        // indepStatic10 = static positions (baseline 3-voice cost);
        // indepMotion10 = constellation motion at full spread (adds HRIR churn).
        // Spread is in this test's normalized 0..1 setter space (1.0 = 100%).
        {"indepStatic10", 10, true, true, 1, 0, 0.0f},
        {"indepMotion10", 10, true, true, 1, 1, 1.0f},
    };

    const auto springCaseCount = cases.size();
    for (size_t i = 0; i < springCaseCount; ++i)
    {
        auto cloud = cases[i];
        cloud.name += "_cloud";
        cloud.reverbEngine = 1;
        cases.push_back(std::move(cloud));
    }

    std::vector<CpuCaseSummary> summaries;
    summaries.reserve(cases.size());

    for (const auto& cfg : cases)
    {
        CpuCaseSummary summary;
        summary.name = cfg.name;
        summary.entries.reserve(sampleRates.size() * blockSizes.size());

        for (double fs : sampleRates)
        {
            for (int blockSize : blockSizes)
            {
                processor.prepareToPlay(fs, blockSize);
                applyStressParams(cfg);
                if (!configurationErrors.isEmpty())
                    return "[ERROR] CpuStressProfile configuration: " + configurationErrors.joinIntoString("; ");

                juce::AudioBuffer<float> buffer(2, blockSize);
                juce::MidiBuffer midi;

                const int totalSamples = static_cast<int>(fs * durationSec);
                const int totalBlocks = (totalSamples + blockSize - 1) / blockSize;
                const int processedSamples = totalBlocks * blockSize;

                // Precompute sustained audio outside the timed region. A DC
                // input decays through the high-pass filters and understates
                // the work of the noise-disabled configurations.
                std::vector<float> stimulus(static_cast<size_t>(processedSamples));
                for (int n = 0; n < processedSamples; ++n)
                {
                    const double phase = juce::MathConstants<double>::twoPi * n / fs;
                    stimulus[static_cast<size_t>(n)] =
                        static_cast<float>(0.09 * std::sin(phase * 173.0) + 0.06 * std::sin(phase * 997.0) +
                                           0.03 * std::sin(phase * 4001.0));
                }
                stimulus.front() += 0.8f;

                std::array<float, kStressRepeats> cpuRuns{};
                float peak = 0.0f;
                bool finiteOk = true;
                float selectedCpu = -1.0f;
                EchoesModel::StageTimingSnapshot selectedTiming{};
                bool selectedHasTiming = false;

                for (int rep = 0; rep < kStressRepeats; ++rep)
                {
                    processor.enableStageTimingCapture(true);
                    processor.resetStageTimingCapture();

                    for (int i = 0; i < settleBlocks; ++i)
                    {
                        buffer.clear();
                        processor.processBlock(buffer, midi);
                    }

                    processor.resetStageTimingCapture();

                    float repPeak = 0.0f;
                    bool repFiniteOk = true;

                    auto start = std::chrono::high_resolution_clock::now();
                    for (int b = 0; b < totalBlocks; ++b)
                    {
                        auto* left = buffer.getWritePointer(0);
                        auto* right = buffer.getWritePointer(1);
                        for (int i = 0; i < blockSize; ++i)
                        {
                            const float sample = stimulus[static_cast<size_t>(b * blockSize + i)];
                            left[i] = sample;
                            right[i] = sample;
                        }

                        processor.processBlock(buffer, midi);

                        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                        {
                            const float* data = buffer.getReadPointer(ch);
                            for (int i = 0; i < buffer.getNumSamples(); ++i)
                            {
                                const float v = data[i];
                                if (!std::isfinite(v))
                                    repFiniteOk = false;
                                repPeak = std::max(repPeak, std::abs(v));
                            }
                        }
                    }
                    auto end = std::chrono::high_resolution_clock::now();

                    const std::chrono::duration<double> elapsed = end - start;
                    const double realTime = static_cast<double>(processedSamples) / fs;
                    const float cpuPct =
                        (realTime > 0.0) ? static_cast<float>(elapsed.count() / realTime * 100.0) : 0.0f;
                    cpuRuns[static_cast<size_t>(rep)] = cpuPct;

                    EchoesModel::StageTimingSnapshot runTiming{};
                    const bool hasRunTiming = processor.getStageTimingSnapshot(runTiming);
                    processor.enableStageTimingCapture(false);

                    if (cpuPct >= selectedCpu)
                    {
                        selectedCpu = cpuPct;
                        selectedTiming = runTiming;
                        selectedHasTiming = hasRunTiming;
                    }

                    peak = std::max(peak, repPeak);
                    finiteOk = finiteOk && repFiniteOk;
                }

                std::sort(cpuRuns.begin(), cpuRuns.end());
                const float cpuMin = cpuRuns.front();
                const float cpuMax = cpuRuns.back();
                const float cpuPct = cpuRuns[kStressRepeats / 2];

                CpuProfileEntry entry{cfg.name, fs, blockSize, cpuPct, cpuMin, cpuMax, peak, finiteOk};
                entry.stageTiming = selectedTiming;
                entry.hasStageTiming = selectedHasTiming;
                summary.entries.push_back(entry);

                if (cpuPct > summary.maxCpu)
                {
                    summary.maxCpu = cpuPct;
                    summary.maxFs = fs;
                    summary.maxBlock = blockSize;
                }
                summary.maxPeak = std::max(summary.maxPeak, peak);
                summary.finiteAll = summary.finiteAll && finiteOk;
                if (cpuPct > 100.0f)
                    ++summary.overBudget;
                if ((cpuMax - cpuMin) > kCpuVarianceBudgetPct)
                    ++summary.unstable;
            }
        }

        summaries.push_back(std::move(summary));
    }

    float globalMaxCpu = 0.0f;
    juce::String globalMaxCase;
    double globalMaxFs = 0.0;
    int globalMaxBlock = 0;
    const CpuProfileEntry* globalMaxEntry = nullptr;

    for (const auto& summary : summaries)
    {
        for (const auto& entry : summary.entries)
        {
            if (entry.cpuPct > globalMaxCpu)
            {
                globalMaxCpu = entry.cpuPct;
                globalMaxCase = summary.name;
                globalMaxFs = entry.sampleRate;
                globalMaxBlock = entry.blockSize;
                globalMaxEntry = &entry;
            }
        }
    }

    juce::String result;
    result << "CpuStressProfile:";
    for (const auto& summary : summaries)
    {
        const bool hasSignal = summary.maxPeak > 1.0e-4f;
        const bool passed = summary.finiteAll && hasSignal && (summary.overBudget == 0) && (summary.unstable == 0);

        result << "\n  " << summary.name << ":";
        for (const auto& entry : summary.entries)
        {
            result << " " << juce::String(entry.sampleRate / 1000.0, 1) << "k/" << entry.blockSize << "="
                   << juce::String(entry.cpuPct, 1) << "%"
                   << "(" << juce::String(entry.cpuMinPct, 1) << "-" << juce::String(entry.cpuMaxPct, 1) << ")";
        }
        result << " max=" << juce::String(summary.maxCpu, 1) << "%@" << juce::String(summary.maxFs / 1000.0, 1) << "k/"
               << summary.maxBlock << " over=" << summary.overBudget << " unstable=" << summary.unstable
               << " finite=" << (summary.finiteAll ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    }

    result << "\n  worst=" << globalMaxCase << " max=" << juce::String(globalMaxCpu, 1) << "%@"
           << juce::String(globalMaxFs / 1000.0, 1) << "k/" << globalMaxBlock;

    if (globalMaxEntry && globalMaxEntry->hasStageTiming && globalMaxEntry->stageTiming.blocks > 0)
    {
        const auto& timing = globalMaxEntry->stageTiming;
        const uint64_t totalUs = timing.totalUs;
        const double avgBlockUs = (timing.blocks > 0) ? (double)totalUs / (double)timing.blocks : 0.0;
        const uint64_t measuredUs = timing.preampUs + timing.reverbUs + timing.delayUs + timing.toneUs + timing.mixUs +
                                    timing.noiseUs + timing.binauralUs;
        const uint64_t otherUs = (totalUs > measuredUs) ? (totalUs - measuredUs) : 0;
        auto pct = [&](uint64_t us) -> double { return (totalUs > 0) ? (100.0 * (double)us / (double)totalUs) : 0.0; };

        result << "\n  breakdown(" << globalMaxEntry->caseName << " " << juce::String(globalMaxFs / 1000.0, 1) << "k/"
               << globalMaxEntry->blockSize << "):"
               << " avg=" << juce::String(avgBlockUs, 1) << "us"
               << " max=" << juce::String((double)timing.maxBlockUs, 0) << "us"
               << " preamp=" << juce::String(pct(timing.preampUs), 1) << "%"
               << " reverb=" << juce::String(pct(timing.reverbUs), 1) << "%"
               << " delay=" << juce::String(pct(timing.delayUs), 1) << "%"
               << " tone=" << juce::String(pct(timing.toneUs), 1) << "%"
               << " mix=" << juce::String(pct(timing.mixUs), 1) << "%"
               << " noise=" << juce::String(pct(timing.noiseUs), 1) << "%"
               << " binaural=" << juce::String(pct(timing.binauralUs), 1) << "%"
               << " other=" << juce::String(pct(otherUs), 1) << "%";
    }
    return result;
}

juce::String runDenormalHandlingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Test verifies no denormal numbers are produced and CPU usage
    // doesn't spike when processing near-zero signals.

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int segmentSamples = static_cast<int>(fs * 10.0f);
    const int segmentBlocks = (segmentSamples + blkSize - 1) / blkSize;

    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    ReverbMacroParams reverbMacros;
    testModel.UpdateParameters(0.5f, 0.5f, 0.2f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.5f,
                               0.5f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                               0.5f, 0, 0, -1.0f, -1.0f, 1.0f, 1.0f, true, 0, 1.0f, 1.0f, 0.0f, reverbMacros);

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    auto runSegment = [&](float inputValue, float& cpuPct, int& denormals)
    {
        // Settle
        for (int w = 0; w < settleBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        denormals = 0;
        juce::ScopedNoDenormals noDenormals;
        auto start = std::chrono::high_resolution_clock::now();
        int processedSamples = 0;

        for (int b = 0; b < segmentBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), inputValue);
            std::fill(buf[1].begin(), buf[1].end(), inputValue);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize && processedSamples < segmentSamples; ++s, ++processedSamples)
            {
                const float v0 = buf[0][static_cast<size_t>(s)];
                const float v1 = buf[1][static_cast<size_t>(s)];
                if (std::fpclassify(v0) == FP_SUBNORMAL || std::fpclassify(v1) == FP_SUBNORMAL)
                    denormals++;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double> elapsed = end - start;
        const double realTime = static_cast<double>(segmentSamples) / fs;
        cpuPct = (realTime > 0.0) ? static_cast<float>(elapsed.count() / realTime * 100.0) : 0.0f;
    };

    // Other work on the machine only ever adds time, so alternate the two
    // inputs over several rounds and keep each one's fastest run.
    constexpr int rounds = 3;
    float cpuSilence = std::numeric_limits<float>::max(), cpuNear = std::numeric_limits<float>::max();
    int denormSilence = 0, denormNear = 0;
    for (int round = 0; round < rounds; ++round)
    {
        float cpu = 0.0f;
        int denormals = 0;
        runSegment(0.0f, cpu, denormals);
        cpuSilence = std::min(cpuSilence, cpu);
        denormSilence += denormals;
        runSegment(1.0e-10f, cpu, denormals);
        cpuNear = std::min(cpuNear, cpu);
        denormNear += denormals;
    }

    // Denormal arithmetic costs many times normal arithmetic, so near-silence
    // must run within 25% (or one point) of silence. No absolute CPU budget:
    // that measures how busy the machine is, not denormal handling.
    const float cpuRatio = (cpuSilence > 0.0f) ? (cpuNear / cpuSilence) : cpuNear;
    const bool cpuOk = (cpuRatio <= 1.25f) || (cpuNear <= cpuSilence + 1.0f);
    const bool passed = (denormSilence == 0 && denormNear == 0 && cpuOk);

    juce::String result;
    result << "Denormals: cpu=" << juce::String(cpuSilence, 2) << "%/" << juce::String(cpuNear, 2)
           << "% ratio=" << juce::String(cpuRatio, 2) << " denorms=" << denormSilence << "/" << denormNear << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runOutputProtectionTest(SpatialEchoAudioProcessor& processor)
{
    auto& vts = processor.getValueTreeState();
    auto set = [&](const char* id, float normalized)
    {
        if (auto* parameter = vts.getParameter(id))
            parameter->setValueNotifyingHost(normalized);
    };
    set("Mix", 0.0f);
    set("OutputGain", 0.5f);
    set("InputLevel", 0.5f);
    set("Power", 1.0f);
    bool passed = true;
    float smallestUnprotectedPeak = std::numeric_limits<float>::max();
    float smallestProtectedPeak = std::numeric_limits<float>::max();
    float largestProtectedPeak = 0.0f;
    int cases = 0;
    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        for (int callback : {1, 64, 257})
        {
            for (bool enabled : {false, true})
            {
                set("OutputProtection", enabled ? 1.0f : 0.0f);
                processor.prepareToPlay(rate, callback);
                juce::AudioBuffer<float> buffer(2, callback);
                juce::MidiBuffer midi;
                std::array<float, 2> peaks{};
                const int count = static_cast<int>(rate * 0.08);
                for (int offset = 0; offset < count; offset += callback)
                {
                    for (int i = 0; i < callback; ++i)
                    {
                        const auto phase = juce::MathConstants<double>::twoPi * 197.0 * (offset + i) / rate;
                        buffer.setSample(0, i, 1.8f * static_cast<float>(std::sin(phase)));
                        buffer.setSample(1, i, -1.4f * static_cast<float>(std::sin(phase + 0.31)));
                    }
                    processor.processBlock(buffer, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < callback; ++i)
                        {
                            const float value = buffer.getSample(ch, i);
                            passed = passed && std::isfinite(value);
                            peaks[static_cast<size_t>(ch)] = std::max(peaks[static_cast<size_t>(ch)], std::abs(value));
                        }
                }
                for (float peak : peaks)
                {
                    if (enabled)
                    {
                        largestProtectedPeak = std::max(largestProtectedPeak, peak);
                        smallestProtectedPeak = std::min(smallestProtectedPeak, peak);
                        passed = passed && peak > 0.9f && peak <= 1.000001f;
                    }
                    else
                    {
                        smallestUnprotectedPeak = std::min(smallestUnprotectedPeak, peak);
                        passed = passed && peak > 1.2f;
                    }
                }
                ++cases;
            }
        }
    }
    return "OutputProtection: cases=" + juce::String(cases) +
           " protectedPeakRange=" + juce::String(smallestProtectedPeak, 6) + ".." +
           juce::String(largestProtectedPeak, 6) +
           " minimumUnprotectedPeak=" + juce::String(smallestUnprotectedPeak, 6) + (passed ? " [PASS]" : " [FAIL]");
}

// Output Protection is a signal-path switch, not a gain: enabling it routes the
// output through an oversampled nonlinear limiter, disabling it bypasses that
// path entirely. Toggling it therefore must be crossfaded; a hard switch at a
// block boundary steps the signal discontinuously.
//
// The test drives a full-dry sine hot enough to engage the limiter knee, so the
// protected and unprotected paths genuinely differ, then flips the parameter
// mid-render and looks for a sample-to-sample jump that the signal itself cannot
// explain. It self-checks that the two paths actually differ, so it fails loudly
// rather than passing vacuously if the drive level ever stops engaging the knee.
juce::String runOutputProtectionToggleContinuityTest(SpatialEchoAudioProcessor& processor)
{
    const float fs = 48000.0f;
    const int blockSize = 64;
    const int totalBlocks = 96;
    const int toggleBlock = 48;

    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };

    // Mix=0 isolates the dry path so the only nonlinearity under test is the
    // output protection stage itself, not the delay/reverb voicing.
    setFloatParam("Mix", 0.0f);
    setFloatParam("InputLevel", 1.0f);
    setFloatParam("OutputGain", 1.0f);
    setBoolParam("MixLock", false);
    setBoolParam("Power", true);

    // Render the sine, optionally flipping OutputProtection at toggleBlock.
    auto render = [&](bool startOn, bool toggleAt, std::vector<float>& out)
    {
        setBoolParam("OutputProtection", startOn);
        processor.prepareToPlay(fs, blockSize);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;
        out.clear();
        out.reserve(static_cast<size_t>(totalBlocks * blockSize));

        double phase = 0.0;
        const double inc = juce::MathConstants<double>::twoPi * 997.0 / fs;
        constexpr float kDrive = 1.35f;  // above the 0.96 knee after unity gain

        for (int b = 0; b < totalBlocks; ++b)
        {
            if (toggleAt && b == toggleBlock)
                setBoolParam("OutputProtection", !startOn);

            for (int i = 0; i < blockSize; ++i)
            {
                const float v = kDrive * (float)std::sin(phase);
                phase += inc;
                if (phase >= juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, v);
            }

            processor.processBlock(buffer, midi);

            const float* data = buffer.getReadPointer(0);
            for (int i = 0; i < blockSize; ++i)
                out.push_back(data[i]);
        }
    };

    std::vector<float> steadyOn, steadyOff, toggled;
    render(true, false, steadyOn);
    render(false, false, steadyOff);
    render(true, true, toggled);

    bool finiteOk = true;
    for (const auto& v : {std::cref(steadyOn), std::cref(steadyOff), std::cref(toggled)})
        for (float s : v.get())
            if (!std::isfinite(s))
                finiteOk = false;

    // Self-check: the two paths must actually differ, or this test proves nothing.
    float pathDiff = 0.0f;
    const int compareStart = toggleBlock * blockSize;
    for (int i = compareStart; i < (int)steadyOn.size(); ++i)
        pathDiff = std::max(pathDiff, std::abs(steadyOn[(size_t)i] - steadyOff[(size_t)i]));
    const bool meaningful = pathDiff > 0.02f;

    // Largest adjacent-sample step the signal itself produces, measured well away
    // from the toggle, then the largest step across the toggle window.
    auto maxStep = [&](const std::vector<float>& v, int from, int to)
    {
        float m = 0.0f;
        for (int i = std::max(1, from); i < std::min(to, (int)v.size()); ++i)
            m = std::max(m, std::abs(v[(size_t)i] - v[(size_t)(i - 1)]));
        return m;
    };

    const int toggleIdx = toggleBlock * blockSize;
    const float referenceStep = maxStep(toggled, blockSize * 8, toggleIdx - blockSize);
    const float toggleStep = maxStep(toggled, toggleIdx - 2, toggleIdx + blockSize * 4);

    // A crossfaded switch stays within the signal's own slew. A hard switch jumps
    // by roughly the path difference, which is far outside it.
    const float allowed = referenceStep * 1.5f + 1.0e-4f;
    const bool continuous = toggleStep <= allowed;
    const bool passed = finiteOk && meaningful && continuous;

    juce::String result;
    result << "OutputProtectionToggleContinuity: pathDiff=" << juce::String(pathDiff, 4)
           << " referenceStep=" << juce::String(referenceStep, 5) << " toggleStep=" << juce::String(toggleStep, 5)
           << " allowed=" << juce::String(allowed, 5) << " meaningful=" << (meaningful ? "yes" : "no")
           << " finite=" << (finiteOk ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

// Auto-panic watchdog contract (see SpatialEchoAudioProcessor::runAutoPanicWatchdog):
//  1. immunity - max-feedback self-oscillation and hot-but-deliberate input never
//     trigger, with measured headroom to the threshold;
//  2. runaway - a sustained loud fault on the wet bus triggers, then re-triggers
//     on the cooldown cadence while the fault persists;
//  3. NaN wedge - corrupting the tape loop triggers the hard flush and the wet
//     path actually recovers (the pre-watchdog behavior was a permanently
//     silent wet chain, masked by the output NaN scrub);
//  4. gating - with AutoPanic off the watchdog never fires.
juce::String runAutoPanicTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 256;
    const int blocksPerSecond = static_cast<int>(fs) / blockSize;

    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
            p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };
    auto setPlainFloatParam = [&](const char* id, float value)
    {
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(value));
    };

    setFloatParam("InputLevel", 0.5f);
    setFloatParam("Intensity", 0.4f);
    setFloatParam("RepeatRate", 0.5f);
    setFloatParam("Mix", 0.5f);
    setFloatParam("OutputGain", 0.5f);
    setBoolParam("EchoSend", true);
    setBoolParam("Power", true);
    setBoolParam("AutoPanic", true);
    setBoolParam("OutputProtection", false);  // observe the bus level unclipped
    setBoolParam("SpatialEnabled", false);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    double phase = 0.0;
    const double phaseInc = juce::MathConstants<double>::twoPi * 440.0 / fs;

    struct BlockStats
    {
        float peak = 0.0f;
        float rms = 0.0f;
        bool finite = true;
    };
    auto runBlock = [&](float inputAmplitude) -> BlockStats
    {
        buffer.clear();
        if (inputAmplitude != 0.0f)
        {
            for (int s = 0; s < blockSize; ++s)
            {
                const float v = inputAmplitude * static_cast<float>(std::sin(phase));
                phase += phaseInc;
                buffer.setSample(0, s, v);
                buffer.setSample(1, s, v);
            }
        }
        processor.processBlock(buffer, midi);

        BlockStats stats;
        double sumSq = 0.0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int s = 0; s < blockSize; ++s)
            {
                const float v = data[s];
                if (!std::isfinite(v))
                    stats.finite = false;
                else
                {
                    stats.peak = std::max(stats.peak, std::abs(v));
                    sumSq += static_cast<double>(v) * static_cast<double>(v);
                }
            }
        }
        stats.rms = static_cast<float>(std::sqrt(sumSq / (2.0 * blockSize)));
        return stats;
    };
    uint32_t presentationNoiseState = 0x5E1F05C1u;
    auto runPresentationNoiseBlock = [&](bool excite) -> BlockStats
    {
        buffer.clear();
        if (excite)
        {
            for (int s = 0; s < blockSize; ++s)
            {
                presentationNoiseState = presentationNoiseState * 1664525u + 1013904223u;
                const float noise = (static_cast<float>(presentationNoiseState >> 8) / 8388608.0f) - 1.0f;
                const float value = 0.2511886432f * noise;  // -12 dBFS peak
                buffer.setSample(0, s, value);
                buffer.setSample(1, s, value);
            }
        }
        processor.processBlock(buffer, midi);

        BlockStats stats;
        double sumSq = 0.0;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int s = 0; s < blockSize; ++s)
            {
                const float value = data[s];
                if (!std::isfinite(value))
                    stats.finite = false;
                else
                {
                    stats.peak = std::max(stats.peak, std::abs(value));
                    sumSq += static_cast<double>(value) * static_cast<double>(value);
                }
            }
        }
        stats.rms = static_cast<float>(std::sqrt(sumSq / (2.0 * blockSize)));
        return stats;
    };

    juce::StringArray failures;

    // ---- Step 0: baseline ----
    float steadyRms = 0.0f;
    for (int b = 0; b < 2 * blocksPerSecond; ++b)
    {
        const auto stats = runBlock(0.25f);
        if (b >= blocksPerSecond)  // average the second half
            steadyRms += stats.rms;
    }
    steadyRms /= static_cast<float>(blocksPerSecond);
    if (steadyRms < 1.0e-4f)
        failures.add("baseline produced no signal");
    if (processor.getAutoPanicEventCount() != 0)
        failures.add("baseline triggered auto-panic");

    // ---- Step 1: self-oscillation immunity + threshold headroom ----
    setFloatParam("Intensity", 1.0f);
    float selfOscPeak = 0.0f;
    for (int b = 0; b < 5 * blocksPerSecond; ++b)  // driven self-osc
        selfOscPeak = std::max(selfOscPeak, runBlock(0.25f).peak);
    for (int b = 0; b < 4 * blocksPerSecond; ++b)  // free-running bed, no input
        selfOscPeak = std::max(selfOscPeak, runBlock(0.0f).peak);
    if (processor.getAutoPanicEventCount() != 0)
        failures.add("self-oscillation triggered auto-panic");
    // Demand real safety margin between the loudest legit self-osc level and
    // the absolute trigger threshold, so tuning drift gets caught here.
    if (selfOscPeak * 2.0f > SpatialEchoAudioProcessor::kAutoPanicPeakThreshold)
        failures.add("insufficient headroom: selfOscPeak=" + juce::String(selfOscPeak, 3));

    // ---- Step 2: hot-but-deliberate input immunity (relative gate) ----
    setFloatParam("Intensity", 0.4f);
    for (int b = 0; b < 2 * blocksPerSecond; ++b)
        runBlock(4.0f);  // +12 dB host signal
    if (processor.getAutoPanicEventCount() != 0)
        failures.add("hot input triggered auto-panic");

    // ---- Step 3: sustained runaway triggers on the cooldown cadence ----
    // Expected timeline: hold 0.4 s -> event 1; cooldown 2 s + hold -> event 2
    // at ~2.8 s; event 3 would land at ~5.2 s, outside the 5 s window.
    processor.debugSetWetBusFault(50.0f);
    for (int b = 0; b < 5 * blocksPerSecond; ++b)
        runBlock(0.0f);
    const auto runawayEvents = processor.getAutoPanicEventCount();
    if (runawayEvents < 2 || runawayEvents > 3)
        failures.add("runaway events=" + juce::String(static_cast<int>(runawayEvents)) + " (expected 2-3)");
    processor.debugSetWetBusFault(0.0f);
    for (int b = 0; b < 3 * blocksPerSecond; ++b)
        runBlock(0.25f);
    if (processor.getAutoPanicEventCount() != runawayEvents)
        failures.add("watchdog kept firing after fault removed");

    // ---- Step 4: NaN wedge -> hard flush -> wet path recovers ----
    const auto eventsBeforeNaN = processor.getAutoPanicEventCount();
    processor.debugPoisonTapeLoop(std::numeric_limits<float>::quiet_NaN());
    bool nanTriggered = false;
    for (int b = 0; b < blocksPerSecond && !nanTriggered; ++b)
    {
        runBlock(0.25f);
        nanTriggered = processor.getAutoPanicEventCount() > eventsBeforeNaN;
    }
    if (!nanTriggered)
        failures.add("NaN wedge did not trigger auto-panic");
    bool recoveredFinite = true;
    float recoveredRms = 0.0f;
    for (int b = 0; b < 2 * blocksPerSecond; ++b)
    {
        const auto stats = runBlock(0.25f);
        if (b >= (3 * blocksPerSecond) / 2)
        {
            recoveredFinite = recoveredFinite && stats.finite;
            recoveredRms += stats.rms;
        }
    }
    recoveredRms /= static_cast<float>(blocksPerSecond / 2);
    if (!recoveredFinite)
        failures.add("output not finite after NaN flush");
    if (recoveredRms < 0.25f * steadyRms)
        failures.add("wet path did not recover after NaN flush: rms=" + juce::String(recoveredRms, 5) +
                     " baseline=" + juce::String(steadyRms, 5));
    if (processor.getEchoBranchMeterLevel() < 1.0e-3f)
        failures.add("echo branch stayed dead after NaN flush");

    // ---- Step 5: mode off gates the watchdog ----
    setBoolParam("AutoPanic", false);
    const auto eventsBeforeOff = processor.getAutoPanicEventCount();
    processor.debugPoisonTapeLoop(std::numeric_limits<float>::quiet_NaN());
    for (int b = 0; b < 2 * blocksPerSecond; ++b)
        runBlock(0.25f);
    if (processor.getAutoPanicEventCount() != eventsBeforeOff)
        failures.add("watchdog fired while AutoPanic was off");

    // ---- Step 6: public max-level self-oscillation remains musical ----
    // Echo Level is a static presentation control, not loop gain. A legitimate
    // bounded tail must not be classified as runaway merely because the user
    // turns that public output trim to its documented +12 dB maximum.
    struct PresentationRunStats
    {
        float peak = 0.0f;
        float lateRms = 0.0f;
        float echoLevelReadbackDb = -999.0f;
        float runawayNormalization = 1.0f;
        float normalizedPeak = 0.0f;
        uint32_t watchdogEvents = 0;
        bool finite = true;
    };
    auto runPresentationCase = [&](float echoLevelDb)
    {
        processor.prepareToPlay(fs, blockSize);
        setBoolParam("AutoPanic", true);
        setBoolParam("OutputProtection", false);
        setBoolParam("EchoSend", true);
        setBoolParam("Power", true);
        setBoolParam("SpatialEnabled", false);
        setFloatParam("Mix", 1.0f);
        setFloatParam("Intensity", 1.0f);
        setPlainFloatParam("EchoLevel", echoLevelDb);
        presentationNoiseState = 0x5E1F05C1u;

        PresentationRunStats runStats;
        runStats.echoLevelReadbackDb =
            vts.getRawParameterValue("EchoLevel") != nullptr ? vts.getRawParameterValue("EchoLevel")->load() : -999.0f;
        const auto eventsBefore = processor.getAutoPanicEventCount();
        double lateEnergy = 0.0;
        int lateValueCount = 0;
        for (int b = 0; b < blocksPerSecond / 2; ++b)
        {
            const auto stats = runPresentationNoiseBlock(true);
            runStats.peak = std::max(runStats.peak, stats.peak);
            runStats.finite = runStats.finite && stats.finite;
            runStats.runawayNormalization =
                std::max(runStats.runawayNormalization, processor.getAutoPanicRunawayMonitorNormalization());
        }
        for (int b = 0; b < 8 * blocksPerSecond; ++b)
        {
            const auto stats = runPresentationNoiseBlock(false);
            runStats.peak = std::max(runStats.peak, stats.peak);
            runStats.finite = runStats.finite && stats.finite;
            runStats.runawayNormalization =
                std::max(runStats.runawayNormalization, processor.getAutoPanicRunawayMonitorNormalization());
            if (b >= 6 * blocksPerSecond)
            {
                lateEnergy += static_cast<double>(stats.rms) * stats.rms * (2 * blockSize);
                lateValueCount += 2 * blockSize;
            }
        }
        runStats.lateRms = lateValueCount > 0 ? static_cast<float>(std::sqrt(lateEnergy / lateValueCount)) : 0.0f;
        runStats.normalizedPeak = runStats.peak / std::max(1.0f, runStats.runawayNormalization);
        runStats.watchdogEvents = processor.getAutoPanicEventCount() - eventsBefore;
        return runStats;
    };

    const auto zeroDbPresentation = runPresentationCase(0.0f);
    const auto maxPresentation = runPresentationCase(12.0f);
    const float presentationGainRatio = maxPresentation.lateRms / std::max(1.0e-9f, zeroDbPresentation.lateRms);
    const float presentationGainErrorDb =
        20.0f * std::log10(std::max(1.0e-9f, presentationGainRatio / juce::Decibels::decibelsToGain(12.0f)));

    const float maxPresentationEchoLevelReadback = maxPresentation.echoLevelReadbackDb;
    const float maxPresentationPeak = maxPresentation.peak;
    const float maxPresentationNormalizedPeak = maxPresentation.normalizedPeak;
    const float maxPresentationLateRms = maxPresentation.lateRms;
    const auto maxPresentationEvents = maxPresentation.watchdogEvents;
    if (std::abs(maxPresentationEchoLevelReadback - 12.0f) > 0.05f)
        failures.add("max public Echo Level write did not reach +12 dB: readback=" +
                     juce::String(maxPresentationEchoLevelReadback, 2));
    if (!zeroDbPresentation.finite || !maxPresentation.finite)
        failures.add("max public Echo Level self-oscillation became non-finite");
    if (zeroDbPresentation.lateRms < 0.1f)
        failures.add("0 dB Echo Level reference lost its late tail: rms=" +
                     juce::String(zeroDbPresentation.lateRms, 4));
    if (std::abs(presentationGainErrorDb) > 0.10f)
        failures.add(
            "+12 dB Echo Level was not applied to the rendered tail: ratio=" + juce::String(presentationGainRatio, 4) +
            " error=" + juce::String(presentationGainErrorDb, 3) + " dB");
    if (maxPresentationLateRms < 0.1f)
        failures.add("max public Echo Level self-oscillation lost its late tail: rms=" +
                     juce::String(maxPresentationLateRms, 4));
    // The clean-room saturator ceiling can keep the bounded public output
    // below the detector's raw threshold. Crossing that threshold is not a
    // safety contract: the normalized monitor must stay below it with zero
    // events, while the injected runaway in step 3 must still trigger.
    if (maxPresentationNormalizedPeak >= SpatialEchoAudioProcessor::kAutoPanicPeakThreshold)
        failures.add("runaway monitor still saw bounded max public Echo Level as unstable: normalizedPeak=" +
                     juce::String(maxPresentationNormalizedPeak, 3) +
                     " threshold=" + juce::String(SpatialEchoAudioProcessor::kAutoPanicPeakThreshold, 3));
    if (maxPresentationEvents != 0)
        failures.add("max public Echo Level flushed bounded self-oscillation: events=" +
                     juce::String(static_cast<int>(maxPresentationEvents)));

    // Restore defaults and rebuild clean DSP state for whatever runs next.
    setPlainFloatParam("EchoLevel", 0.0f);
    setFloatParam("Mix", 0.5f);
    setFloatParam("Intensity", 0.4f);
    setBoolParam("AutoPanic", true);
    processor.prepareToPlay(fs, blockSize);

    juce::String result;
    result << "AutoPanic: selfOscPeak=" << juce::String(selfOscPeak, 3)
           << " maxPresentationPeak=" << juce::String(maxPresentationPeak, 3)
           << " maxPresentationNormalizedPeak=" << juce::String(maxPresentationNormalizedPeak, 3)
           << " maxPresentationLateRms=" << juce::String(maxPresentationLateRms, 3)
           << " presentationGainRatio=" << juce::String(presentationGainRatio, 3) << "x/"
           << juce::String(presentationGainErrorDb, 3) << "dB error"
           << " echoLevelReadback=" << juce::String(maxPresentationEchoLevelReadback, 2) << "dB"
           << " maxPresentationEvents=" << juce::String(static_cast<int>(maxPresentationEvents))
           << " runawayEvents=" << juce::String(static_cast<int>(runawayEvents))
           << " recoveredRms=" << juce::String(recoveredRms, 5);
    if (failures.isEmpty())
        result << " [PASS]";
    else
        result << " [FAIL] " << failures.joinIntoString("; ");
    return result;
}

juce::String runOutputGainAutomationSmoothingTest(SpatialEchoAudioProcessor& processor)
{
    constexpr double fs = 48000.0;
    constexpr int blockSize = 128;
    constexpr int warmupBlocks = 6;
    constexpr int settleBlocks = 22;
    constexpr float amplitude = 0.20f;
    constexpr float frequency = 997.0f;

    processor.prepareToPlay(fs, blockSize);

    auto& vts = processor.getValueTreeState();
    auto setFloatParam = [&](const char* id, float value)
    {
        if (auto* p = vts.getParameter(id))
            p->setValueNotifyingHost(p->convertTo0to1(value));
    };
    auto setBoolParam = [&](const char* id, bool value)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
            p->setValueNotifyingHost(value ? 1.0f : 0.0f);
    };

    setFloatParam("Mix", 0.0f);
    setFloatParam("OutputGain", 0.5f);
    setBoolParam("OutputProtection", false);
    setBoolParam("SpatialEnabled", false);
    setBoolParam("Power", true);

    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    std::array<float, blockSize> input{};
    double phase = 0.61;
    const double phaseInc = juce::MathConstants<double>::twoPi * static_cast<double>(frequency) / fs;

    auto fillInput = [&]()
    {
        buffer.clear();
        for (int s = 0; s < blockSize; ++s)
        {
            const float x = amplitude * static_cast<float>(std::sin(phase));
            phase += phaseInc;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            input[static_cast<size_t>(s)] = x;
            buffer.setSample(0, s, x);
            buffer.setSample(1, s, x);
        }
    };

    auto renderBlock = [&]()
    {
        fillInput();
        processor.processBlock(buffer, midi);
    };

    auto averageGain = [&](int start, int end) -> float
    {
        double sum = 0.0;
        int count = 0;
        for (int s = juce::jlimit(0, blockSize, start); s < juce::jlimit(0, blockSize, end); ++s)
        {
            const float in = input[static_cast<size_t>(s)];
            if (std::abs(in) <= 0.025f)
                continue;

            const float out = buffer.getSample(0, s);
            sum += static_cast<double>(std::abs(out / in));
            ++count;
        }

        return (count > 0) ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
    };

    bool finiteOk = true;
    auto updateFinite = [&]()
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const float* data = buffer.getReadPointer(ch);
            for (int s = 0; s < buffer.getNumSamples(); ++s)
                finiteOk = finiteOk && std::isfinite(data[s]);
        }
    };

    for (int i = 0; i < warmupBlocks; ++i)
    {
        renderBlock();
        updateFinite();
    }

    setFloatParam("OutputGain", 1.0f);
    renderBlock();
    updateFinite();
    const float firstWindowGain = averageGain(0, 32);
    const float lastWindowGain = averageGain(blockSize - 32, blockSize);

    float settledGain = 0.0f;
    for (int i = 0; i < settleBlocks; ++i)
    {
        renderBlock();
        updateFinite();
        if (i == settleBlocks - 1)
            settledGain = averageGain(blockSize - 32, blockSize);
    }

    const float targetGain = juce::Decibels::decibelsToGain(12.0f);
    const bool firstWindowNotStepped = firstWindowGain <= 1.60f;
    const bool rampVisible = lastWindowGain >= firstWindowGain * 1.06f;
    const bool settlesToTarget = std::abs(settledGain - targetGain) <= targetGain * 0.08f;
    const bool passed = finiteOk && firstWindowNotStepped && rampVisible && settlesToTarget;

    juce::String result;
    result << "OutputGainAutomationSmoothing:"
           << " first=" << juce::String(firstWindowGain, 3) << " last=" << juce::String(lastWindowGain, 3)
           << " settled=" << juce::String(settledGain, 3) << " target=" << juce::String(targetGain, 3)
           << " finite=" << (finiteOk ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runProcessorLifecycleTeardownTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // The rate and block-size choices below repeat every 12 iterations, so 12
    // covers every combination for both passes. SPATIAL_ECHO_LIFECYCLE_ITERS
    // raises the count for a longer soak.
    const int iterations = juce::jlimit(
        1, 512, juce::SystemStats::getEnvironmentVariable("SPATIAL_ECHO_LIFECYCLE_ITERS", "12").getIntValue());

    constexpr std::array<double, 3> sampleRates = {44100.0, 48000.0, 96000.0};
    constexpr std::array<int, 4> blockSizes = {64, 128, 256, 512};

    bool finiteOk = true;
    bool signalSeen = false;
    float peak = 0.0f;
    int prepareCalls = 0;

    auto runPass = [&](SpatialEchoAudioProcessor& local, double fs, int blockSize, int iter, int passIndex)
    {
        local.prepareToPlay(fs, blockSize);
        ++prepareCalls;

        auto& vts = local.getValueTreeState();
        auto setFloatParam = [&](const char* id, float value)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterFloat*>(vts.getParameter(id)))
                p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
        };
        auto setBoolParam = [&](const char* id, bool value)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterBool*>(vts.getParameter(id)))
                p->setValueNotifyingHost(value ? 1.0f : 0.0f);
        };
        auto setChoiceParam = [&](const char* id, int index)
        {
            if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(vts.getParameter(id)))
            {
                const int maxIdx = std::max(0, p->choices.size() - 1);
                const int clamped = std::max(0, std::min(index, maxIdx));
                const float norm = (maxIdx > 0) ? static_cast<float>(clamped) / static_cast<float>(maxIdx) : 0.0f;
                p->setValueNotifyingHost(norm);
            }
        };

        const bool reverbHeavy = ((iter + passIndex) % 2) == 0;
        setChoiceParam("DelaySetting", reverbHeavy ? 10 : 11);
        setFloatParam("ReverbSize", reverbHeavy ? 0.9f : 0.6f);
        setFloatParam("ReverbDecay", reverbHeavy ? 0.9f : 0.6f);
        setFloatParam("ReverbCharacter", reverbHeavy ? 0.9f : 0.5f);
        setFloatParam("ReverbMod", reverbHeavy ? 0.8f : 0.2f);
        setFloatParam("ReverbWidth", reverbHeavy ? 0.8f : 0.0f);
        setFloatParam("Mix", 1.0f);
        setFloatParam("InputLevel", 1.0f);
        setFloatParam("Intensity", 1.0f);
        setBoolParam("OutputProtection", true);
        setBoolParam("SpatialEnabled", (passIndex % 2) == 0);

        juce::AudioBuffer<float> buffer(2, blockSize);
        juce::MidiBuffer midi;

        for (int block = 0; block < 6; ++block)
        {
            buffer.clear();
            if (block == 0)
            {
                const float impulse = reverbHeavy ? 0.9f : 0.7f;
                buffer.setSample(0, 0, impulse);
                buffer.setSample(1, 0, impulse);
            }

            local.processBlock(buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* data = buffer.getReadPointer(ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float v = data[i];
                    finiteOk = finiteOk && std::isfinite(v);
                    peak = std::max(peak, std::abs(v));
                    signalSeen = signalSeen || (std::abs(v) > 1.0e-6f);
                }
            }
        }
    };

    for (int iter = 0; iter < iterations; ++iter)
    {
        SpatialEchoAudioProcessor local;

        const double fsA = sampleRates[static_cast<size_t>(iter % static_cast<int>(sampleRates.size()))];
        const int bsA = blockSizes[static_cast<size_t>((iter * 3) % static_cast<int>(blockSizes.size()))];
        runPass(local, fsA, bsA, iter, 0);

        const double fsB = sampleRates[static_cast<size_t>((iter + 1) % static_cast<int>(sampleRates.size()))];
        const int bsB = blockSizes[static_cast<size_t>((iter * 5 + 1) % static_cast<int>(blockSizes.size()))];
        runPass(local, fsB, bsB, iter, 1);

        local.releaseResources();
    }

    const bool passed = finiteOk && signalSeen && (prepareCalls == iterations * 2);
    juce::String result;
    result << "ProcessorLifecycleTeardown: iters=" << iterations << " prepares=" << prepareCalls
           << " peak=" << juce::String(peak, 4) << " finite=" << (finiteOk ? "yes" : "no")
           << " signal=" << (signalSeen ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runInvalidInputRecoveryTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // One NaN/Inf sample from a host or upstream plug-in must not latch every
    // recursive filter and feedback loop. Without this guard, with Auto-Panic
    // off (the default), the output scrub would hold the unit silent until it
    // is re-prepared.
    constexpr double fs = 48000.0;
    constexpr int blockSize = 512;
    constexpr int totalBlocks = 282;  // ~3 s
    constexpr int badBlock = 94;      // ~1 s in
    constexpr int tailBlocks = 47;    // last ~0.5 s

    auto render = [&](bool injectInvalid, bool& finite)
    {
        SpatialEchoAudioProcessor processor;
        processor.prepareToPlay(fs, blockSize);
        processor.loadFactoryPreset(FactoryPresetLibrary::kFactoryPresets.front().name);

        juce::Random rng(0x0BADF00D);
        juce::AudioBuffer<float> block(2, blockSize);
        juce::MidiBuffer midi;
        double tailEnergy = 0.0;
        finite = true;
        for (int b = 0; b < totalBlocks; ++b)
        {
            for (int s = 0; s < blockSize; ++s)
            {
                const bool burst = ((b * blockSize + s) % 24000) < 4000;
                const float v = burst ? 0.5f * (rng.nextFloat() * 2.0f - 1.0f) : 0.0f;
                block.setSample(0, s, v);
                block.setSample(1, s, v);
            }
            if (injectInvalid && b == badBlock)
            {
                block.setSample(0, 10, std::numeric_limits<float>::quiet_NaN());
                block.setSample(1, 20, std::numeric_limits<float>::infinity());
                block.setSample(0, 30, -1.0e30f);
            }
            processor.processBlock(block, midi);
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < blockSize; ++s)
                {
                    const float y = block.getSample(ch, s);
                    finite = finite && std::isfinite(y);
                    if (b >= totalBlocks - tailBlocks)
                        tailEnergy += static_cast<double>(y) * y;
                }
            }
        }
        return 10.0 * std::log10(juce::jmax(1.0e-24, tailEnergy / (2.0 * tailBlocks * blockSize)));
    };

    bool cleanFinite = false, badFinite = false;
    const double cleanTailDb = render(false, cleanFinite);
    const double badTailDb = render(true, badFinite);
    const bool cleanAudible = cleanTailDb > -80.0;
    const bool recovered = badTailDb > cleanTailDb - 6.0;
    const bool passed = cleanFinite && badFinite && cleanAudible && recovered;

    juce::String result;
    result << "InvalidInputRecovery:"
           << " cleanTail=" << juce::String(cleanTailDb, 1) << "dB"
           << " afterInvalidTail=" << juce::String(badTailDb, 1) << "dB (needs > clean - 6)"
           << " finite=" << ((cleanFinite && badFinite) ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
