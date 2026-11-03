// ToneStackTests: Unit tests for tone EQ (boost/cut, frequency response, spectral shape, interaction).

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "ToneStackTests.h"
#include "../../PluginProcessor.h"
#include "../../DSP/EchoesModel.h"
#include "../../DSP/ToneFiltering.h"
#include "../../DSP/dependencies/RbjBiquad.h"
#include "../Common/TestUtilities.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace SpatialEchoTests::ToneStack
{

namespace
{

//==============================================================================
// Shared shelf-response measurement used by the reciprocity gate below and by
// the low/high boost tests.
//
// Drives a bare ToneStackProcessor (not EchoesModel - the tape path around it
// is nonlinear and would make a magnitude measurement meaningless) with a unit
// sine and returns the steady-state magnitude at that frequency from a
// single-bin DFT.
//
// The analysis window is exactly one second at 48 kHz and the block size
// divides it exactly, so every integer-Hz probe frequency sits on a bin centre
// and there is no spectral leakage to trade against.
constexpr float kShelfProbeFs = 48000.0f;
constexpr int kShelfProbeBlock = 480;   // divides 48000 exactly
constexpr int kShelfProbeBlocks = 100;  // 48000 samples = 1.0 s
constexpr int kShelfWarmBlocks = 50;    // 0.5 s: 20 ms smoother + settle

double measureShelfMagnitude(double freq, float bassKnob, float trebleKnob)
{
    ToneStackProcessor tone;
    tone.Reset(kShelfProbeFs, 1);
    tone.UpdateParameters(bassKnob, trebleKnob, 1.0f);

    std::vector<std::vector<float>> buffer(1, std::vector<float>(kShelfProbeBlock, 0.0f));

    const double twoPi = 2.0 * 3.14159265358979323846;
    long long n = 0;

    auto fillAndProcess = [&]()
    {
        for (int i = 0; i < kShelfProbeBlock; ++i)
            buffer[0][static_cast<size_t>(i)] = static_cast<float>(
                std::sin(twoPi * freq * static_cast<double>(n + i) / static_cast<double>(kShelfProbeFs)));
        tone.UpdateParameters(bassKnob, trebleKnob, 1.0f);
        tone.ProcessBuffer(buffer, kShelfProbeBlock);
    };

    for (int b = 0; b < kShelfWarmBlocks; ++b)
    {
        fillAndProcess();
        n += kShelfProbeBlock;
    }

    double re = 0.0, im = 0.0;
    for (int b = 0; b < kShelfProbeBlocks; ++b)
    {
        fillAndProcess();
        for (int i = 0; i < kShelfProbeBlock; ++i)
        {
            const double ph = twoPi * freq * static_cast<double>(n + i) / static_cast<double>(kShelfProbeFs);
            const double y = static_cast<double>(buffer[0][static_cast<size_t>(i)]);
            re += y * std::cos(ph);
            im += y * std::sin(ph);
        }
        n += kShelfProbeBlock;
    }

    const double N = static_cast<double>(kShelfProbeBlocks) * kShelfProbeBlock;
    return 2.0 * std::sqrt(re * re + im * im) / N;
}

double shelfMagnitudeDb(double freq, float bassKnob, float trebleKnob)
{
    return 20.0 * std::log10(std::max(1.0e-12, measureShelfMagnitude(freq, bassKnob, trebleKnob)));
}

// Worst |boost + cut| in dB over the given probe frequencies, for knob
// positions symmetric about 0.5. Zero means a cut exactly undoes a boost.
// `isBass` selects which knob is swept; the other is pinned at 0.5, where its
// shelf is bit-exactly unity (y = lp + 1*hp = x).
double worstShelfReciprocityDb(const std::vector<double>& probes, bool isBass, double& worstFreqOut,
                               float& worstKnobOut)
{
    double worst = 0.0;
    worstFreqOut = 0.0;
    worstKnobOut = 0.5f;

    for (float knob : {1.0f, 0.85f, 0.65f})
    {
        const float mirrored = 1.0f - knob;
        for (double f : probes)
        {
            const double boostDb = isBass ? shelfMagnitudeDb(f, knob, 0.5f) : shelfMagnitudeDb(f, 0.5f, knob);
            const double cutDb = isBass ? shelfMagnitudeDb(f, mirrored, 0.5f) : shelfMagnitudeDb(f, 0.5f, mirrored);
            const double sum = std::abs(boostDb + cutDb);
            if (sum > worst)
            {
                worst = sum;
                worstFreqOut = f;
                worstKnobOut = knob;
            }
        }
    }
    return worst;
}

}  // namespace

juce::String runLowBoostTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Compare low frequency energy at min vs max low EQ
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 30;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float lfEnergyMin = 0.0f, lfEnergyMax = 0.0f;

    for (int lowTest = 0; lowTest < 2; ++lowTest)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float low = (lowTest == 0) ? 0.0f : 1.0f;
        testModel.UpdateParameters(low, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

        for (int w = 0; w < ballisticsBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        // Send impulse
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = 0.9f;
        buf[1][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        // Measure total energy (low EQ affects overall level too)
        float energy = 0.0f;
        for (int b = 0; b < measureBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int s = 0; s < blkSize; ++s)
                energy += buf[0][static_cast<size_t>(s)] * buf[0][static_cast<size_t>(s)];
        }

        if (lowTest == 0)
            lfEnergyMin = energy;
        else
            lfEnergyMax = energy;
    }

    float ratio = (lfEnergyMin > 0.0001f) ? lfEnergyMax / lfEnergyMin : 0.0f;

    // Bass-shelf boost/cut reciprocity, measured directly on the tone stack.
    //
    // This rides in the low-boost test on purpose: the dedicated
    // runToneShelfReciprocityTest below is registered as ToneShelfReciprocity,
    // and keeping the assertion here as well keeps the property gated if that
    // registration is ever dropped.
    //
    // If the shelf placed its RC pole at fc in both directions, the boost
    // transition would sit between fc and G*fc while the cut transition sat
    // between fc/G and fc. Measured at 48 kHz on the 100 Hz bass shelf, that
    // gives +7.4036 dB at 100 Hz against only -2.5964 dB at full cut - a
    // reciprocity error of 4.8073 dB. The pole sits at fc/sqrt(G), which makes
    // cut the exact reciprocal of boost at every frequency: 0.00002 dB through
    // this harness. With the pole at fc the harness measures 4.80725 dB and a
    // 7.4036 dB midpoint, so the gate does fail on that design. 0.05 dB sits
    // ~2500x above the measured floor and ~96x below the defect.
    double worstFreq = 0.0;
    float worstKnob = 0.5f;
    const double worstRecipDb = worstShelfReciprocityDb({50.0, 100.0, 200.0, 400.0}, true, worstFreq, worstKnob);
    const double recipToleranceDb = 0.05;

    // Shelf-midpoint property: at fc the gain must be exactly half the knob's
    // dB (+5.00 dB here; a pole at fc would give +7.40 dB).
    const double midpointDb = shelfMagnitudeDb(100.0, 1.0f, 0.5f);
    const double midpointErrDb = std::abs(midpointDb - 5.0);
    const double midpointToleranceDb = 0.25;

    // Max low should have measurable difference from min low
    bool passed = lfEnergyMin > 0.0001f && lfEnergyMax > 0.0001f && (ratio > 1.02f || ratio < 0.98f) &&
                  worstRecipDb <= recipToleranceDb && midpointErrDb <= midpointToleranceDb;

    juce::String result;
    result << "LowBoost: min=" << juce::String(lfEnergyMin * 1000.0f, 2) << ", "
           << "max=" << juce::String(lfEnergyMax * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(ratio, 2) << ", "
           << "worstRecip=" << juce::String(worstRecipDb, 5) << "dB@" << juce::String(worstFreq, 0) << "Hz/knob"
           << juce::String(worstKnob, 2) << " (tol " << juce::String(recipToleranceDb, 3) << "), "
           << "midpoint@100Hz=" << juce::String(midpointDb, 4) << "dB (want 5.0000 +/- "
           << juce::String(midpointToleranceDb, 2) << ") " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runHighBoostTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Compare high frequency energy at min vs max high EQ
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 30;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float hfEnergyMin = 0.0f, hfEnergyMax = 0.0f;

    for (int highTest = 0; highTest < 2; ++highTest)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float high = (highTest == 0) ? 0.0f : 1.0f;
        testModel.UpdateParameters(0.5f, high, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

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

        // Measure HF energy using differentiator
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

        if (highTest == 0)
            hfEnergyMin = hfEnergy;
        else
            hfEnergyMax = hfEnergy;
    }

    float ratio = (hfEnergyMin > 0.0001f) ? hfEnergyMax / hfEnergyMin : 0.0f;

    // Treble-shelf boost/cut reciprocity. Same reasoning as the bass assertion
    // in runLowBoostTest above; with its pole at fc the 5 kHz high shelf would
    // measure +7.4036 dB boost against -2.5964 dB cut, so the pole sits at
    // fc*sqrt(G).
    double worstFreq = 0.0;
    float worstKnob = 0.5f;
    const double worstRecipDb = worstShelfReciprocityDb({1000.0, 2500.0, 5000.0, 10000.0}, false, worstFreq, worstKnob);
    const double recipToleranceDb = 0.05;

    const double midpointDb = shelfMagnitudeDb(5000.0, 0.5f, 1.0f);
    const double midpointErrDb = std::abs(midpointDb - 5.0);
    const double midpointToleranceDb = 0.25;

    // Max high should have measurable difference from min high
    bool passed = hfEnergyMin > 0.0001f && hfEnergyMax > 0.0001f && (ratio > 1.02f || ratio < 0.98f) &&
                  worstRecipDb <= recipToleranceDb && midpointErrDb <= midpointToleranceDb;

    juce::String result;
    result << "HighBoost: min=" << juce::String(hfEnergyMin * 1000.0f, 2) << ", "
           << "max=" << juce::String(hfEnergyMax * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(ratio, 2) << ", "
           << "worstRecip=" << juce::String(worstRecipDb, 5) << "dB@" << juce::String(worstFreq, 0) << "Hz/knob"
           << juce::String(worstKnob, 2) << " (tol " << juce::String(recipToleranceDb, 3) << "), "
           << "midpoint@5kHz=" << juce::String(midpointDb, 4) << "dB (want 5.0000 +/- "
           << juce::String(midpointToleranceDb, 2) << ") " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runToneInteractionTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Compare output with both controls at min vs both at max
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;
    const int measureBlocks = 30;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float energyBothMin = 0.0f, energyBothMax = 0.0f;

    for (int test = 0; test < 2; ++test)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        float lowHigh = (test == 0) ? 0.0f : 1.0f;
        testModel.UpdateParameters(lowHigh, lowHigh, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

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

        if (test == 0)
            energyBothMin = energy;
        else
            energyBothMax = energy;
    }

    float ratio = (energyBothMin > 0.0001f) ? energyBothMax / energyBothMin : 0.0f;

    // Both max should have measurable difference from both min
    bool passed = energyBothMin > 0.0001f && energyBothMax > 0.0001f && (ratio > 1.02f || ratio < 0.98f);

    juce::String result;
    result << "Interaction: min=" << juce::String(energyBothMin * 1000.0f, 2) << ", "
           << "max=" << juce::String(energyBothMax * 1000.0f, 2) << ", "
           << "ratio=" << juce::String(ratio, 2) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runToneBypassTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP test: Verify signal passes through at neutral (0.5) settings
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int ballisticsBlocks = 100;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    // Neutral settings: low=0.5, high=0.5
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

    // Find peak of first echo
    float echoPeak = 0.0f;
    for (int b = 0; b < 20; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        for (int s = 0; s < blkSize; ++s)
            echoPeak = std::max(echoPeak, std::abs(buf[0][static_cast<size_t>(s)]));
    }

    float attenDb = 20.0f * std::log10(std::max(0.0001f, echoPeak / inputPeak));

    // At neutral, signal should pass through with some attenuation (record chain)
    bool passed = echoPeak > 0.01f;

    juce::String result;
    result << "ToneBypass: in=" << juce::String(inputPeak, 2) << ", "
           << "echo=" << juce::String(echoPeak, 3) << ", "
           << "atten=" << juce::String(attenDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runToneAutomationSmoothingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 64;
    constexpr int warmBlocks = 80;
    constexpr int transitionBlocks = 48;
    constexpr float freq = 5000.0f;
    constexpr float amp = 0.1f;

    ToneStackProcessor tone;
    tone.Reset(fs, 1);

    std::vector<std::vector<float>> buffer(1, std::vector<float>(blockSize, 0.0f));
    float phase = 0.0f;
    const float phaseInc = 2.0f * juce::MathConstants<float>::pi * freq / fs;
    float previous = 0.0f;
    bool havePrevious = false;
    bool finite = true;
    float maxDelta = 0.0f;

    auto fillTone = [&]()
    {
        for (int i = 0; i < blockSize; ++i)
        {
            buffer[0][static_cast<size_t>(i)] = amp * std::sin(phase);
            phase += phaseInc;
            if (phase >= 2.0f * juce::MathConstants<float>::twoPi)
                phase -= 2.0f * juce::MathConstants<float>::twoPi;
        }
    };

    auto processBlock = [&](float treble) -> float
    {
        tone.UpdateParameters(0.5f, treble, 1.0f);
        fillTone();
        tone.ProcessBuffer(buffer, blockSize);

        double sumSq = 0.0;
        for (int i = 0; i < blockSize; ++i)
        {
            const float sample = buffer[0][static_cast<size_t>(i)];
            if (!std::isfinite(sample))
                finite = false;
            if (havePrevious)
                maxDelta = std::max(maxDelta, std::abs(sample - previous));
            previous = sample;
            havePrevious = true;
            sumSq += static_cast<double>(sample) * static_cast<double>(sample);
        }

        return static_cast<float>(std::sqrt(sumSq / static_cast<double>(blockSize)));
    };

    double lowRmsSum = 0.0;
    int lowRmsCount = 0;
    for (int b = 0; b < warmBlocks; ++b)
    {
        const float rms = processBlock(0.0f);
        if (b >= warmBlocks - 8)
        {
            lowRmsSum += rms;
            ++lowRmsCount;
        }
    }

    const float lowRms = (lowRmsCount > 0) ? static_cast<float>(lowRmsSum / static_cast<double>(lowRmsCount)) : 0.0f;

    float firstRms = 0.0f;
    float midRms = 0.0f;
    double finalRmsSum = 0.0;
    int finalRmsCount = 0;

    for (int b = 0; b < transitionBlocks; ++b)
    {
        const float rms = processBlock(1.0f);
        if (b == 0)
            firstRms = rms;
        if (b == 15)
            midRms = rms;
        if (b >= transitionBlocks - 8)
        {
            finalRmsSum += rms;
            ++finalRmsCount;
        }
    }

    const float finalRms =
        (finalRmsCount > 0) ? static_cast<float>(finalRmsSum / static_cast<double>(finalRmsCount)) : 0.0f;

    auto ratioDb = [](float numerator, float denominator)
    { return 20.0f * std::log10(std::max(1.0e-9f, numerator) / std::max(1.0e-9f, denominator)); };

    const float firstLiftDb = ratioDb(firstRms, lowRms);
    const float midLiftDb = ratioDb(midRms, lowRms);
    const float finalLiftDb = ratioDb(finalRms, lowRms);

    int failures = 0;
    if (!finite)
        ++failures;
    if (lowRms <= 1.0e-5f || finalLiftDb < 10.0f)
        ++failures;
    if (firstLiftDb > finalLiftDb - 4.0f)
        ++failures;
    if (midLiftDb < firstLiftDb + 3.0f)
        ++failures;
    if (maxDelta > 0.6f)
        ++failures;

    const bool passed = failures == 0;
    juce::String result;
    result << "ToneAutomationSmoothing:"
           << " first=" << juce::String(firstLiftDb, 1) << "dB"
           << " mid=" << juce::String(midLiftDb, 1) << "dB"
           << " final=" << juce::String(finalLiftDb, 1) << "dB"
           << " maxDelta=" << juce::String(maxDelta, 4) << " finite=" << (finite ? "yes" : "no")
           << " failures=" << failures << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runToneBufferBoundsContractTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 16;

    ToneStackProcessor tone;
    tone.Reset(fs, 1);
    tone.UpdateParameters(0.5f, 0.5f, 1.0f);
    std::vector<std::vector<float>> extraChannel(2, std::vector<float>(blockSize, 0.125f));
    const auto extraChannelBefore = extraChannel;
    tone.ProcessBuffer(extraChannel, blockSize);
    const bool channelMismatchNoop = extraChannel == extraChannelBefore;

    tone.Reset(fs, 2);
    tone.UpdateParameters(0.5f, 0.5f, 1.0f);
    std::vector<std::vector<float>> shortChannel{
        std::vector<float>(blockSize, -0.125f),
        std::vector<float>(blockSize / 2, 0.25f),
    };
    const auto shortChannelBefore = shortChannel;
    tone.ProcessBuffer(shortChannel, blockSize);
    const bool shortChannelNoop = shortChannel == shortChannelBefore;

    const bool passed = channelMismatchNoop && shortChannelNoop;
    juce::String result;
    result << "ToneBufferBoundsContract: channels=" << (channelMismatchNoop ? "safe" : "changed")
           << " short=" << (shortChannelNoop ? "safe" : "changed") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Biquad low-frequency numerics gate.
//
// Guards the shared Biquad primitive against float32 numerics: with float32
// coefficients and float32 Direct Form I state, a lowpass whose DC gain must be
// exactly 1.0 does not deliver it.
//
// A float32 build of the filter, settled 200 s under a unit step and
// cross-checked against H(z=1) evaluated in double from the float32
// coefficients as stored, measures at fs = 48 kHz:
//     fc = 0.5 Hz -> 0.000000   (exactly silent)
//     fc = 1.0 Hz -> 0.000000   (exactly silent)
//     fc = 2.0 Hz -> 2.035964
//     fc = 3.0 Hz -> 0.499717
//     fc = 5.0 Hz -> 1.138486
//     fc = 10  Hz -> 0.932247
// and at fs = 44.1 kHz, fc = 2 Hz reaches 127.86.
//
// Three stacked causes:
// (1) (1 - cos w) underflows to exactly 0 in float32 for fc <= 1.865 Hz at
//     48 kHz (1.713 Hz at 44.1 kHz), zeroing every b coefficient and making
//     the filter silent, not merely inaccurate.
// (2) Storing a1 (approx -2) and a2 (approx +1) in float32 destroys the
//     denominator: a0 + a1 + a2 must resolve a difference of order 1e-8 out of
//     a sum near 2, which is below a float32 ULP there. With the derivation
//     done perfectly in double but the coefficients still stored as float32,
//     the DC gain is 15.99 at 1 Hz and infinite in the analytic limit.
// (3) The float32 DF-I state recursion adds error on top: with the same
//     float32 coefficients but double state, the step response lands exactly on
//     the analytic H(1) (2.000000 at 44.1 kHz / 2 Hz); with float32 state it
//     lands on 127.86 instead.
// Coefficients and state are therefore both double.
//
// This check fails if anyone moves Biquad's coefficients or state back to
// float. It also covers two edge cases that must stay finite: fc at or above
// Nyquist (LPF at
// 26 kHz / 48 kHz diverged to inf) and Reset() before Init() (Q = 0 gave NaN
// coefficients).
//
// Live consumer that motivated this: wowflutter.h designs an LPF at 1.0 Hz.
juce::String runBiquadLowFrequencyNumericsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    using SpatialEchoTests::TestUtilities;

    const float fs = 48000.0f;

    // A lowpass passes DC unchanged, by construction, at every cutoff.
    // 0.5% is far outside float32 rounding but far inside the observed failure
    // (the float32 filter is 100% low at 0.5 and 1 Hz and 104% high at 2 Hz;
    // at 44.1 kHz it reaches 12686% high at 2 Hz).
    const double dcTolerance = 0.005;

    const float cutoffs[] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 1000.0f};

    bool dcGainOk = true;
    double worstErr = 0.0;
    float worstFc = 0.0f;
    double worstGain = 1.0;

    for (float fc : cutoffs)
    {
        RbjBiquad filter;
        filter.Init(BiquadType::LPF, fc, fs, 0.707f, 0.0f);

        // Settle a unit step. The slowest pole here (fc = 0.5 Hz) needs a long
        // run to reach steady state; 3e6 samples is 62.5 s at 48 kHz.
        float settled = 0.0f;
        for (int i = 0; i < 3000000; ++i)
            settled = filter.ProcessSample(1.0f);

        if (!std::isfinite(settled))
        {
            dcGainOk = false;
            worstErr = 1.0e9;
            worstFc = fc;
            worstGain = static_cast<double>(settled);
            break;
        }

        const double err = std::abs(static_cast<double>(settled) - 1.0);
        if (err > worstErr)
        {
            worstErr = err;
            worstFc = fc;
            worstGain = static_cast<double>(settled);
        }
        if (err > dcTolerance)
            dcGainOk = false;
    }

    // Cutoff at or above Nyquist must stay bounded rather than diverge.
    bool nyquistStable = true;
    for (float fc : {24000.0f, 26000.0f, 40000.0f, 96000.0f})
    {
        RbjBiquad filter;
        filter.Init(BiquadType::LPF, fc, fs, 0.707f, 0.0f);
        for (int i = 0; i < 200000; ++i)
        {
            const float y = filter.ProcessSample(i == 0 ? 1.0f : 0.0f);
            if (!std::isfinite(y) || std::abs(y) > 10.0f)
            {
                nyquistStable = false;
                break;
            }
        }
        if (!nyquistStable)
            break;
    }

    // Reset() before Init() must not leave NaN coefficients behind.
    bool resetBeforeInitFinite = true;
    {
        RbjBiquad filter;
        filter.Reset(fs);
        for (int i = 0; i < 64; ++i)
        {
            if (!std::isfinite(filter.ProcessSample(1.0f)))
            {
                resetBeforeInitFinite = false;
                break;
            }
        }
    }

    // A Q of zero must not divide by zero inside the coefficient derivation.
    bool zeroQFinite = true;
    {
        RbjBiquad filter;
        filter.Init(BiquadType::LPF, 1000.0f, fs, 0.0f, 0.0f);
        for (int i = 0; i < 64; ++i)
        {
            if (!std::isfinite(filter.ProcessSample(1.0f)))
            {
                zeroQFinite = false;
                break;
            }
        }
    }

    const bool passed = dcGainOk && nyquistStable && resetBeforeInitFinite && zeroQFinite;

    juce::String details;
    details << "worst LPF DC gain " << juce::String(worstGain, 6) << " at fc=" << juce::String(worstFc, 2) << "Hz"
            << " (err=" << juce::String(worstErr, 6) << ", tol=" << juce::String(dcTolerance, 4) << ")"
            << "; nyquistStable=" << (nyquistStable ? "yes" : "no")
            << "; resetBeforeInitFinite=" << (resetBeforeInitFinite ? "yes" : "no")
            << "; zeroQFinite=" << (zeroQFinite ? "yes" : "no");

    return TestUtilities::formatResult(passed, "BiquadLowFrequencyNumerics", details);
}

//==============================================================================
// Tone stack summation-linearity gate.
//
// EchoesModel::ProcessBuffer runs three separate ToneStackProcessor instances
// over three delay-head buffers with identical parameters and independent
// state, then sums them, and asserts in a comment that this reconstructs the
// Linked echo exactly. That is only true if the tone stack is strictly linear
// AND the three instances' gain smoothers stay in lockstep.
//
// This gate proves both at once: it drives three instances with independent
// random signals and a fourth with their sum, sweeping bass and treble every
// block so the smoothers never settle, and varying the block size the way a
// host would. Measured error with the current code is 4.1e-07 relative
// (-127.7 dB), which is the float32 rounding floor for summing three filter
// outputs.
//
// The 1e-4 threshold is deliberately ~240x looser than the observed floor but
// still far tighter than either failure mode it must catch: introducing any
// nonlinearity breaks superposition outright, and desynchronising one
// instance's smoother by a single extra ProcessBuffer call measures 1.4e-03
// (-57 dB), which this gate fails on.
juce::String runToneStackSumLinearityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    using SpatialEchoTests::TestUtilities;

    const float fs = 48000.0f;
    const int channels = 2;
    const double tolerance = 1.0e-4;

    ToneStackProcessor headA, headB, headC, reference;
    headA.Reset(fs, channels);
    headB.Reset(fs, channels);
    headC.Reset(fs, channels);
    reference.Reset(fs, channels);

    std::mt19937 rng(20260904u);
    std::uniform_real_distribution<float> dist(-0.8f, 0.8f);

    double errorEnergy = 0.0;
    double referenceEnergy = 0.0;
    double worstAbsolute = 0.0;

    for (int block = 0; block < 200; ++block)
    {
        // Vary the block size exactly as a host might, identically for all four.
        const int numSamples = (block % 5 == 0) ? 64 : ((block % 3 == 0) ? 137 : 512);

        // Move both tone controls every block so the smoothers are always running.
        const float low = 0.5f + 0.5f * std::sin(static_cast<float>(block) * 0.11f);
        const float high = 0.5f + 0.5f * std::cos(static_cast<float>(block) * 0.07f);
        headA.UpdateParameters(low, high, 1.0f);
        headB.UpdateParameters(low, high, 1.0f);
        headC.UpdateParameters(low, high, 1.0f);
        reference.UpdateParameters(low, high, 1.0f);

        std::vector<std::vector<float>> bufA(channels, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
        std::vector<std::vector<float>> bufB(channels, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
        std::vector<std::vector<float>> bufC(channels, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
        std::vector<std::vector<float>> bufSum(channels, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float a = dist(rng);
                const float b = dist(rng);
                const float c = dist(rng);
                bufA[(size_t)ch][(size_t)i] = a;
                bufB[(size_t)ch][(size_t)i] = b;
                bufC[(size_t)ch][(size_t)i] = c;
                bufSum[(size_t)ch][(size_t)i] = a + b + c;
            }
        }

        headA.ProcessBuffer(bufA, numSamples);
        headB.ProcessBuffer(bufB, numSamples);
        headC.ProcessBuffer(bufC, numSamples);
        reference.ProcessBuffer(bufSum, numSamples);

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const double summed = (double)bufA[(size_t)ch][(size_t)i] + (double)bufB[(size_t)ch][(size_t)i] +
                                      (double)bufC[(size_t)ch][(size_t)i];
                const double expected = (double)bufSum[(size_t)ch][(size_t)i];
                const double error = summed - expected;

                errorEnergy += error * error;
                referenceEnergy += expected * expected;
                worstAbsolute = std::max(worstAbsolute, std::abs(error));
            }
        }
    }

    const double relativeError = (referenceEnergy > 0.0) ? std::sqrt(errorEnergy / referenceEnergy) : 1.0e9;

    const bool passed = std::isfinite(relativeError) && relativeError < tolerance;

    juce::String details;
    details << "relative RMS error " << juce::String(relativeError, 9) << " (tol " << juce::String(tolerance, 6) << ")"
            << ", worst |err| " << juce::String(worstAbsolute, 9) << ", "
            << juce::String(20.0 * std::log10(std::max(relativeError, 1.0e-12)), 2) << " dB";

    return TestUtilities::formatResult(passed, "ToneStackSumLinearity", details);
}

//==============================================================================
// Tone-stack shelf reciprocity gate (dedicated form).
//
// Registered as "ToneShelfReciprocity". The same assertions also run inside
// runLowBoostTest and runHighBoostTest, so the defect is gated either way.
//
// What it proves: a cut of X dB undoes a boost of X dB exactly. A shelf that
// places its RC pole at the requested corner frequency in both directions
// makes the boost transition span fc..G*fc and the cut transition span
// fc/G..fc. Measured at 48 kHz on the 100 Hz bass shelf: +7.4036 dB at full
// boost against -2.5964 dB at full cut, a 4.8073 dB reciprocity error at the
// corner that stays above 1 dB from 20 Hz to 400 Hz, and the same on the 5 kHz
// treble shelf. The pole sits at the shelf midpoint (fc/sqrt(G) low,
// fc*sqrt(G) high), which makes H_cut = 1/H_boost at every frequency.
//
// The error measured through this harness is 0.00002 dB (bass) and
// 0.00000 dB (treble), with the shelf midpoint landing on 5.0000 dB exactly.
// With the pole at fc the harness measures 4.80725 dB and a 7.4036 dB
// midpoint, so this gate fails on that design. The 0.05 dB threshold sits
// ~2500x above the measured floor and ~96x below the defect.
juce::String runToneShelfReciprocityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    using SpatialEchoTests::TestUtilities;

    const double toleranceDb = 0.05;
    const double midpointToleranceDb = 0.25;

    double bassWorstFreq = 0.0, trebleWorstFreq = 0.0;
    float bassWorstKnob = 0.5f, trebleWorstKnob = 0.5f;

    const double bassWorst =
        worstShelfReciprocityDb({20.0, 50.0, 100.0, 200.0, 400.0, 1000.0}, true, bassWorstFreq, bassWorstKnob);
    const double trebleWorst = worstShelfReciprocityDb({500.0, 1000.0, 2500.0, 5000.0, 10000.0, 15000.0}, false,
                                                       trebleWorstFreq, trebleWorstKnob);

    // Shelf-midpoint property: at fc the gain is exactly half the knob's dB.
    const double bassMidDb = shelfMagnitudeDb(100.0, 1.0f, 0.5f);
    const double trebleMidDb = shelfMagnitudeDb(5000.0, 0.5f, 1.0f);
    const double bassMidErr = std::abs(bassMidDb - 5.0);
    const double trebleMidErr = std::abs(trebleMidDb - 5.0);

    // Unity knob must be exactly transparent, not merely close: with G = 1 the
    // complementary split reconstructs the input (hp + lp = x).
    const double unityDb = shelfMagnitudeDb(100.0, 0.5f, 0.5f);
    const bool unityTransparent = std::abs(unityDb) <= 0.01;

    const bool passed = bassWorst <= toleranceDb && trebleWorst <= toleranceDb && bassMidErr <= midpointToleranceDb &&
                        trebleMidErr <= midpointToleranceDb && unityTransparent;

    juce::String details;
    details << "bass worst |boost+cut| " << juce::String(bassWorst, 5) << " dB at " << juce::String(bassWorstFreq, 0)
            << " Hz / knob " << juce::String(bassWorstKnob, 2) << "; treble worst " << juce::String(trebleWorst, 5)
            << " dB at " << juce::String(trebleWorstFreq, 0) << " Hz / knob " << juce::String(trebleWorstKnob, 2)
            << " (tol " << juce::String(toleranceDb, 3) << ")"
            << "; midpoint bass@100Hz " << juce::String(bassMidDb, 4) << " dB, treble@5kHz "
            << juce::String(trebleMidDb, 4) << " dB (want 5.0000 +/- " << juce::String(midpointToleranceDb, 2) << ")"
            << "; unity@0.5 " << juce::String(unityDb, 5) << " dB";

    return TestUtilities::formatResult(passed, "ToneShelfReciprocity", details);
}

}  // namespace SpatialEchoTests::ToneStack

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
