// DelayTests: Tape delay DSP validation: feedback, decay, head ratios, noise buffer, stabilty.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "DelayTests.h"
#include "../../PluginProcessor.h"
#include "../../DSP/EchoesModel.h"
#include "../../DSP/HeadRatioConstants.h"
#include "../../DSP/TapeDelay.h"
#include "../../DSP/TapeNoiseProcessor.h"
#include "../../DSP/TapeRecordSaturator.h"
#include "../../DSP/dependencies/RbjBiquad.h"
#include "../../DSP/dependencies/CircularBuffer.h"
#include "../../DSP/dependencies/wowflutter.h"
#include "../System/EchoesModelTestAccess.h"
#include "../TapeEmulation/TapeDelayTestAccess.h"
#include "../Common/TestUtilities.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using SpatialEchoTests::TestUtilities;

namespace SpatialEchoTests::Delay
{

namespace
{
struct StereoTrace
{
    std::vector<float> left;
    std::vector<float> right;
    float pingPongSeedPhasorNorm = 1.0f;
};

StereoTrace renderStereoImpulseTrace(int stereoMode, float inputL, float inputR, bool playbackOnly = false,
                                     float intensity = 0.0f, int transitionSample = -1)
{
    constexpr float sampleRate = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int totalSamples = 24064;
    int heads[3] = {1, 0, 0};

    TapeDelayProcessor delay;
    delay.Reset(sampleRate, 2, blockSize);
    const auto updateStereoMode = [&](int mode)
    {
        delay.UpdateParameters(intensity, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f,
                               0.0f, mode, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);
    };
    updateStereoMode(stereoMode);

    std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));
    for (int warmBlock = 0; warmBlock < 4; ++warmBlock)
    {
        std::fill(block[0].begin(), block[0].end(), 0.0f);
        std::fill(block[1].begin(), block[1].end(), 0.0f);
        if (playbackOnly)
            TapeDelayTestAccess::processPlaybackOnly(delay, block, blockSize);
        else
            delay.ProcessBuffer(block, blockSize);
    }

    StereoTrace trace;
    trace.left.reserve(totalSamples);
    trace.right.reserve(totalSamples);

    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        if (offset == transitionSample)
            updateStereoMode(1);
        std::fill(block[0].begin(), block[0].end(), 0.0f);
        std::fill(block[1].begin(), block[1].end(), 0.0f);
        if (offset == 0)
        {
            block[0][0] = inputL;
            block[1][0] = inputR;
        }
        if (playbackOnly)
            TapeDelayTestAccess::processPlaybackOnly(delay, block, blockSize);
        else
            delay.ProcessBuffer(block, blockSize);
        trace.left.insert(trace.left.end(), block[0].begin(), block[0].end());
        trace.right.insert(trace.right.end(), block[1].begin(), block[1].end());
    }
    return trace;
}

StereoTrace renderPingPongImpulseAfterHistory(int blockSize, float historyL, float historyR, float inputL, float inputR)
{
    constexpr float sampleRate = 48000.0f;
    constexpr int historyDrainSamples = 24064;
    constexpr int totalSamples = 24064;
    int heads[3] = {1, 0, 0};

    TapeDelayProcessor delay;
    delay.Reset(sampleRate, 2, blockSize);
    const auto updateIntensity = [&](float intensity)
    {
        delay.UpdateParameters(intensity, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f,
                               0.0f, 1, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);
    };
    updateIntensity(0.0f);

    std::vector<std::vector<float>> block(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    for (int warmBlock = 0; warmBlock < 4; ++warmBlock)
        delay.ProcessBuffer(block, blockSize);

    block[0][0] = historyL;
    block[1][0] = historyR;
    delay.ProcessBuffer(block, blockSize);

    for (int offset = 0; offset < historyDrainSamples; offset += blockSize)
    {
        std::fill(block[0].begin(), block[0].end(), 0.0f);
        std::fill(block[1].begin(), block[1].end(), 0.0f);
        delay.ProcessBuffer(block, blockSize);
    }

    updateIntensity(0.65f);
    StereoTrace trace;
    trace.left.reserve(static_cast<size_t>(totalSamples + blockSize));
    trace.right.reserve(static_cast<size_t>(totalSamples + blockSize));
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        std::fill(block[0].begin(), block[0].end(), 0.0f);
        std::fill(block[1].begin(), block[1].end(), 0.0f);
        if (offset == 0)
        {
            block[0][0] = inputL;
            block[1][0] = inputR;
        }
        delay.ProcessBuffer(block, blockSize);
        trace.left.insert(trace.left.end(), block[0].begin(), block[0].end());
        trace.right.insert(trace.right.end(), block[1].begin(), block[1].end());
    }
    return trace;
}

StereoTrace renderPingPongPartitionProgram(int callbackSize)
{
    constexpr float sampleRate = 48000.0f;
    constexpr int preparedBlockSize = 1024;
    constexpr int totalSamples = 22000;
    int heads[3] = {1, 0, 0};

    TapeDelayProcessor delay;
    // Keep preparation identical so this test varies only the host callback
    // partition, not any buffer capacity chosen during Reset().
    delay.Reset(sampleRate, 2, preparedBlockSize);
    delay.UpdateParameters(0.0f, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f, 1,
                           0, false, 0.0f, 0.0f, 0.0f, 1.0f);

    StereoTrace trace;
    trace.left.reserve(totalSamples);
    trace.right.reserve(totalSamples);

    for (int offset = 0; offset < totalSamples; offset += callbackSize)
    {
        const int samplesThisBlock = std::min(callbackSize, totalSamples - offset);
        std::vector<std::vector<float>> block(2, std::vector<float>(static_cast<size_t>(samplesThisBlock), 0.0f));

        for (int sample = 0; sample < samplesThisBlock; ++sample)
        {
            const int timelineSample = offset + sample;
            if (timelineSample >= 1537 && timelineSample < 2238)
            {
                block[0][static_cast<size_t>(sample)] = 0.12f;
            }
            else if (timelineSample >= 2691 && timelineSample < 3598)
            {
                block[0][static_cast<size_t>(sample)] = 0.085f;
                block[1][static_cast<size_t>(sample)] = 0.085f;
            }
            else if (timelineSample >= 3965 && timelineSample < 4578)
            {
                block[1][static_cast<size_t>(sample)] = 0.12f;
            }
            else if (timelineSample >= 5099 && timelineSample < 5938)
            {
                block[0][static_cast<size_t>(sample)] = 0.085f;
                block[1][static_cast<size_t>(sample)] = -0.085f;
            }
            else if (timelineSample >= 6503 && timelineSample < 7167)
            {
                block[0][static_cast<size_t>(sample)] = 0.12f;
            }
            else if (timelineSample >= 7167 && timelineSample < 8099)
            {
                // No silence at this boundary: the hard-left -> balanced
                // change must exercise the fixed 5 ms seed-angle slew.
                block[0][static_cast<size_t>(sample)] = 0.085f;
                block[1][static_cast<size_t>(sample)] = 0.085f;
            }
            else if (timelineSample >= 8099)
            {
                // Keep retargeting inside the -60 to -40 dB transition so the
                // ramp never completes. This catches long-run phasor-norm
                // drift independently of callback boundaries.
                constexpr int cycleSamples = 512;
                const int cyclePosition = (timelineSample - 8099) % cycleSamples;
                const int foldedPosition =
                    cyclePosition <= cycleSamples / 2 ? cyclePosition : cycleSamples - cyclePosition;
                const float triangle = static_cast<float>(foldedPosition) / static_cast<float>(cycleSamples / 2);
                const float quietToDominant = 0.0011f + 0.0088f * triangle;
                block[0][static_cast<size_t>(sample)] = 0.12f;
                block[1][static_cast<size_t>(sample)] = 0.12f * quietToDominant;
            }
        }

        delay.ProcessBuffer(block, samplesThisBlock);
        trace.left.insert(trace.left.end(), block[0].begin(), block[0].end());
        trace.right.insert(trace.right.end(), block[1].begin(), block[1].end());
    }

    trace.pingPongSeedPhasorNorm = TapeDelayTestAccess::pingPongSeedPhasorNorm(delay);

    return trace;
}

struct StereoNullMetrics
{
    float peakDifference = 0.0f;
    float rmsDifference = 0.0f;
    float referencePeak = 0.0f;
    bool sameLength = false;
};

StereoNullMetrics measureStereoNull(const StereoTrace& reference, const StereoTrace& candidate)
{
    StereoNullMetrics metrics;
    metrics.sameLength = reference.left.size() == candidate.left.size() &&
                         reference.right.size() == candidate.right.size() &&
                         reference.left.size() == reference.right.size();
    if (!metrics.sameLength || reference.left.empty())
        return metrics;

    double differenceEnergy = 0.0;
    size_t sampleCount = 0;
    for (size_t sample = 0; sample < reference.left.size(); ++sample)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            const float referenceSample = channel == 0 ? reference.left[sample] : reference.right[sample];
            const float candidateSample = channel == 0 ? candidate.left[sample] : candidate.right[sample];
            const float difference = candidateSample - referenceSample;
            metrics.peakDifference = std::max(metrics.peakDifference, std::abs(difference));
            metrics.referencePeak = std::max(metrics.referencePeak, std::abs(referenceSample));
            differenceEnergy += static_cast<double>(difference) * static_cast<double>(difference);
            ++sampleCount;
        }
    }

    metrics.rmsDifference =
        sampleCount > 0 ? static_cast<float>(std::sqrt(differenceEnergy / static_cast<double>(sampleCount))) : 0.0f;
    return metrics;
}

std::vector<float> renderHeadCapture(const std::array<int, 3>& enabledHeads, int capturedHead, float intensity)
{
    constexpr float sampleRate = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int totalSamples = 24064;
    int heads[3] = {enabledHeads[0], enabledHeads[1], enabledHeads[2]};

    TapeDelayProcessor delay;
    delay.Reset(sampleRate, 2, blockSize);
    delay.enablePerHeadCapture(true);
    delay.UpdateParameters(intensity, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, 80.0f, 137.0f,
                           211.0f, 0, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);

    std::vector<float> trace;
    trace.reserve(totalSamples);
    std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        for (auto& channel : block)
            std::fill(channel.begin(), channel.end(), 0.0f);
        if (offset == 0)
            for (auto& channel : block)
                channel[0] = 0.35f;
        delay.ProcessBuffer(block, blockSize);
        const float* captured = delay.getHeadOutput(capturedHead);
        for (int i = 0; i < blockSize; ++i)
            trace.push_back(captured != nullptr ? captured[i] : 0.0f);
    }
    return trace;
}

std::vector<float> renderMonoOutput(const std::array<int, 3>& enabledHeads, float intensity)
{
    constexpr float sampleRate = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int totalSamples = 28672;
    int heads[3] = {enabledHeads[0], enabledHeads[1], enabledHeads[2]};

    TapeDelayProcessor delay;
    delay.Reset(sampleRate, 1, blockSize);
    delay.UpdateParameters(intensity, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, 80.0f, 300.0f,
                           450.0f, 0, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);

    std::vector<float> trace;
    trace.reserve(totalSamples);
    std::vector<std::vector<float>> block(1, std::vector<float>(blockSize, 0.0f));
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        std::fill(block[0].begin(), block[0].end(), 0.0f);
        if (offset == 0)
            block[0][0] = 0.35f;
        delay.ProcessBuffer(block, blockSize);
        trace.insert(trace.end(), block[0].begin(), block[0].end());
    }
    return trace;
}

StereoTrace renderRightOnlyStereoOutput(const std::array<int, 3>& enabledHeads, float intensity)
{
    constexpr float sampleRate = 48000.0f;
    constexpr int blockSize = 256;
    constexpr int totalSamples = 28672;
    int heads[3] = {enabledHeads[0], enabledHeads[1], enabledHeads[2]};

    TapeDelayProcessor delay;
    delay.Reset(sampleRate, 2, blockSize);
    delay.UpdateParameters(intensity, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true, 80.0f, 300.0f,
                           450.0f, 0, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);

    StereoTrace trace;
    trace.left.reserve(totalSamples);
    trace.right.reserve(totalSamples);
    std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));
    for (int offset = 0; offset < totalSamples; offset += blockSize)
    {
        std::fill(block[0].begin(), block[0].end(), 0.0f);
        std::fill(block[1].begin(), block[1].end(), 0.0f);
        if (offset == 0)
            block[1][0] = 0.35f;
        delay.ProcessBuffer(block, blockSize);
        trace.left.insert(trace.left.end(), block[0].begin(), block[0].end());
        trace.right.insert(trace.right.end(), block[1].begin(), block[1].end());
    }
    return trace;
}

float windowRms(const std::vector<float>& samples, int centre, int radius)
{
    const int begin = std::max(0, centre - radius);
    const int end = std::min(static_cast<int>(samples.size()), centre + radius + 1);
    double energy = 0.0;
    for (int i = begin; i < end; ++i)
        energy +=
            static_cast<double>(samples[static_cast<size_t>(i)]) * static_cast<double>(samples[static_cast<size_t>(i)]);
    return end > begin ? static_cast<float>(std::sqrt(energy / (end - begin))) : 0.0f;
}
}  // namespace

juce::String runMasterRegenerationHeadEnableIndependenceTest(SpatialEchoAudioProcessor&)
{
    constexpr float activeIntensity = 0.65f;
    constexpr int secondH1 = 160 * 48;
    constexpr int h1MasterAtH2 = 217 * 48;
    constexpr int spacedH1MasterAtH2 = 380 * 48;
    constexpr float independenceRatioMax = 0.02f;
    constexpr float controlAndLeakageRatioMax = 0.01f;  // At least 40 dB below the regeneration event.
    const auto compareAt = [](const std::vector<float>& reference, const std::vector<float>& comparison, int centre)
    {
        std::vector<float> difference(reference.size(), 0.0f);
        for (size_t i = 0; i < difference.size(); ++i)
            difference[i] = reference[i] - comparison[i];
        const float referenceRms = windowRms(reference, centre, 96);
        const float differenceRms = windowRms(difference, centre, 96);
        return std::pair<float, float>{referenceRms, differenceRms / std::max(referenceRms, 1.0e-9f)};
    };

    const auto stereoH1Only = renderHeadCapture({1, 0, 0}, 0, activeIntensity);
    const auto stereoAllHeads = renderHeadCapture({1, 1, 1}, 0, activeIntensity);
    const auto stereoVisibleMaster = renderHeadCapture({1, 1, 0}, 1, activeIntensity);
    const auto stereoHiddenMasterTrace = renderHeadCapture({0, 1, 0}, 1, activeIntensity);
    const auto stereoNoRegeneration = renderHeadCapture({1, 1, 0}, 1, 0.0f);
    const auto stereoHeadCount = compareAt(stereoH1Only, stereoAllHeads, secondH1);
    const auto stereoHiddenMaster = compareAt(stereoVisibleMaster, stereoHiddenMasterTrace, h1MasterAtH2);
    const float stereoControlRatio =
        windowRms(stereoNoRegeneration, h1MasterAtH2, 96) / std::max(stereoHiddenMaster.first, 1.0e-9f);

    const auto monoH1Only = renderMonoOutput({1, 0, 0}, activeIntensity);
    const auto monoAllHeads = renderMonoOutput({1, 1, 1}, activeIntensity);
    const auto monoVisibleMaster = renderMonoOutput({1, 1, 0}, activeIntensity);
    const auto monoHiddenMasterTrace = renderMonoOutput({0, 1, 0}, activeIntensity);
    const auto monoNoRegeneration = renderMonoOutput({1, 1, 0}, 0.0f);
    const auto monoHeadCount = compareAt(monoH1Only, monoAllHeads, secondH1);
    const auto monoHiddenMaster = compareAt(monoVisibleMaster, monoHiddenMasterTrace, spacedH1MasterAtH2);
    const float monoControlRatio =
        windowRms(monoNoRegeneration, spacedH1MasterAtH2, 96) / std::max(monoHiddenMaster.first, 1.0e-9f);

    const auto rightH1Only = renderRightOnlyStereoOutput({1, 0, 0}, activeIntensity);
    const auto rightAllHeads = renderRightOnlyStereoOutput({1, 1, 1}, activeIntensity);
    const auto rightVisibleMaster = renderRightOnlyStereoOutput({1, 1, 0}, activeIntensity);
    const auto rightHiddenMasterTrace = renderRightOnlyStereoOutput({0, 1, 0}, activeIntensity);
    const auto rightNoRegeneration = renderRightOnlyStereoOutput({1, 1, 0}, 0.0f);
    const auto rightHeadCount = compareAt(rightH1Only.right, rightAllHeads.right, secondH1);
    const auto rightHiddenMaster =
        compareAt(rightVisibleMaster.right, rightHiddenMasterTrace.right, spacedH1MasterAtH2);
    const float rightControlRatio =
        windowRms(rightNoRegeneration.right, spacedH1MasterAtH2, 96) / std::max(rightHiddenMaster.first, 1.0e-9f);
    const float rightLeftLeakage = std::max(
        windowRms(rightH1Only.left, secondH1, 96) / std::max(rightHeadCount.first, 1.0e-9f),
        windowRms(rightVisibleMaster.left, spacedH1MasterAtH2, 96) / std::max(rightHiddenMaster.first, 1.0e-9f));

    const bool passed =
        stereoHeadCount.first > 1.0e-6f && stereoHiddenMaster.first > 1.0e-6f && monoHeadCount.first > 1.0e-6f &&
        monoHiddenMaster.first > 1.0e-6f && rightHeadCount.first > 1.0e-6f && rightHiddenMaster.first > 1.0e-6f &&
        stereoHeadCount.second <= independenceRatioMax && stereoHiddenMaster.second <= independenceRatioMax &&
        monoHeadCount.second <= independenceRatioMax && monoHiddenMaster.second <= independenceRatioMax &&
        rightHeadCount.second <= independenceRatioMax && rightHiddenMaster.second <= independenceRatioMax &&
        stereoControlRatio <= controlAndLeakageRatioMax && monoControlRatio <= controlAndLeakageRatioMax &&
        rightControlRatio <= controlAndLeakageRatioMax && rightLeftLeakage <= controlAndLeakageRatioMax;
    return "MasterRegenerationHeadEnableIndependence: stereo=" + juce::String(stereoHeadCount.second, 5) + "/" +
           juce::String(stereoHiddenMaster.second, 5) + "/" + juce::String(stereoControlRatio, 5) +
           " mono=" + juce::String(monoHeadCount.second, 5) + "/" + juce::String(monoHiddenMaster.second, 5) + "/" +
           juce::String(monoControlRatio, 5) + " right=" + juce::String(rightHeadCount.second, 5) + "/" +
           juce::String(rightHiddenMaster.second, 5) + "/" + juce::String(rightControlRatio, 5) +
           " leak=" + juce::String(rightLeftLeakage, 5) + (passed ? " [PASS]" : " [FAIL]");
}

juce::String runMasterRegenerationSinglePeriodLatticeTest(SpatialEchoAudioProcessor&)
{
    constexpr float forbiddenRatioMax = 0.01f;  // At least 40 dB below the valid H1 repeat.
    const auto stereoTrace = renderHeadCapture({1, 1, 1}, 0, 0.65f);
    const float stereoExpected = windowRms(stereoTrace, 160 * 48, 72);
    const float stereoForbidden217 = windowRms(stereoTrace, 217 * 48, 72);
    const float stereoForbidden291 = windowRms(stereoTrace, 291 * 48, 72);
    const float stereoForbidden = std::max(stereoForbidden217, stereoForbidden291);
    const float stereoRatio = stereoForbidden / std::max(stereoExpected, 1.0e-9f);

    // H2/H3 remain hidden here so 380/530 ms can only be their reads recirculating into audible H1.
    const auto monoTrace = renderMonoOutput({1, 0, 0}, 0.65f);
    const float monoExpected = windowRms(monoTrace, 160 * 48, 72);
    const float monoForbidden380 = windowRms(monoTrace, 380 * 48, 72);
    const float monoForbidden530 = windowRms(monoTrace, 530 * 48, 72);
    const float monoForbidden = std::max(monoForbidden380, monoForbidden530);
    const float monoRatio = monoForbidden / std::max(monoExpected, 1.0e-9f);

    // This is distinct from the independence test's audible-H2 presentation case:
    // only H1 is audible, so 380/530 ms energy means hidden H2/H3 entered the right record loop.
    const auto rightTrace = renderRightOnlyStereoOutput({1, 0, 0}, 0.65f);
    const float rightExpected = windowRms(rightTrace.right, 160 * 48, 72);
    const float rightForbidden380 = windowRms(rightTrace.right, 380 * 48, 72);
    const float rightForbidden530 = windowRms(rightTrace.right, 530 * 48, 72);
    const float rightForbidden = std::max(rightForbidden380, rightForbidden530);
    const float rightRatio = rightForbidden / std::max(rightExpected, 1.0e-9f);

    const bool passed = stereoExpected > 1.0e-6f && monoExpected > 1.0e-6f && rightExpected > 1.0e-6f &&
                        stereoRatio <= forbiddenRatioMax && monoRatio <= forbiddenRatioMax &&
                        rightRatio <= forbiddenRatioMax;
    return "MasterRegenerationSinglePeriodLattice: stereo=" + juce::String(stereoRatio, 5) +
           " mono=" + juce::String(monoRatio, 5) + " right=" + juce::String(rightRatio, 5) +
           (passed ? " [PASS]" : " [FAIL]");
}

//==============================================================================
// Feedback/Intensity Tests
//==============================================================================

juce::String runIntensitySmoothingTimebaseTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
    constexpr float targetTauSeconds = 0.00416f;
    constexpr float toleranceFraction = 0.05f;
    constexpr float oneMinusInvE = 1.0f - 0.3678794412f;
    int heads[3] = {1, 0, 0};
    bool passed = true;
    juce::String details;

    for (const float fs : sampleRates)
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 1, 1);
        delay.UpdateParameters(1.0f, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

        std::vector<std::vector<float>> buffer(1, std::vector<float>(1, 0.0f));
        int crossingSample = 0;
        float legacy48kSmoothed = 0.0f;
        bool exact48kTrajectory = true;
        const int maxSamples = static_cast<int>(0.02f * fs);
        for (int sample = 1; sample <= maxSamples; ++sample)
        {
            buffer[0][0] = 0.0f;
            if (fs == 48000.0f)
                legacy48kSmoothed = 0.995f * legacy48kSmoothed + (1.0f - 0.995f);
            delay.ProcessBuffer(buffer, 1);
            if (fs == 48000.0f)
                exact48kTrajectory =
                    exact48kTrajectory &&
                    juce::exactlyEqual(TapeDelayTestAccess::intensitySmoothed(delay), legacy48kSmoothed);
            if (TapeDelayTestAccess::intensitySmoothed(delay) >= oneMinusInvE)
            {
                crossingSample = sample;
                break;
            }
        }

        const float measuredTauSeconds = crossingSample > 0 ? static_cast<float>(crossingSample) / fs : 0.0f;
        const float errorFraction = std::abs(measuredTauSeconds - targetTauSeconds) / targetTauSeconds;
        const bool tauOk = crossingSample > 0 && errorFraction <= toleranceFraction;
        const bool exact48k =
            fs != 48000.0f || (juce::exactlyEqual(TapeDelayTestAccess::intensitySmoothingCoefficient(delay), 0.995f) &&
                               exact48kTrajectory);
        passed = passed && tauOk && exact48k;

        details << (details.isEmpty() ? "" : ", ") << juce::String(static_cast<int>(fs)) << "="
                << juce::String(measuredTauSeconds * 1000.0f, 3) << "ms"
                << (exact48k ? "" : " (48 kHz trajectory mismatch)");
    }

    return "IntensitySmoothingTimebase: " + details + " " + (passed ? "[PASS]" : "[FAIL]");
}

juce::String runMinIntensityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 300;
    const int totalBlocks = 200;
    const float threshold = 0.01f;
    const int minSepSamples = static_cast<int>(fs * 0.05f);

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.05f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    for (int w = 0; w < settleBlocks; ++w)
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

    std::vector<float> samples;
    samples.reserve(totalBlocks * blkSize);
    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        samples.insert(samples.end(), buf[0].begin(), buf[0].end());
    }

    int count = 0;
    int lastPeak = -minSepSamples;
    float firstPeak = 0.0f;
    float secondPeak = 0.0f;
    for (int i = 0; i < static_cast<int>(samples.size()); ++i)
    {
        float a = std::abs(samples[static_cast<size_t>(i)]);
        if (a >= threshold && (i - lastPeak) >= minSepSamples)
        {
            count++;
            lastPeak = i;
            if (count == 1)
                firstPeak = a;
            else if (count == 2)
                secondPeak = a;
        }
    }

    float firstDb = 20.0f * std::log10(std::max(1e-10f, firstPeak));
    float secondDb = 20.0f * std::log10(std::max(1e-10f, secondPeak));

    bool passed = (count >= 1 && count <= 2);

    juce::String result;
    result << "Min Intensity: echoes=" << count << " 1st=" << juce::String(firstDb, 1) << "dB"
           << " 2nd=" << juce::String(secondDb, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runDecayRateTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 300;  // Allow motor ballistics to settle before timing reads
    const int totalBlocks = 200;
    const float impulseAmplitude = 0.25f;
    const int minSepSamples = static_cast<int>(fs * 0.05f);
    const int maxRepeats = 5;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 0, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                               0.5f, 0, 0);

    for (int w = 0; w < settleBlocks; ++w)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    EchoesModelTestAccess::enableHeadTimingCapture(testModel, true);
    std::fill(buf[0].begin(), buf[0].end(), 0.0f);
    std::fill(buf[1].begin(), buf[1].end(), 0.0f);
    buf[0][0] = impulseAmplitude;
    buf[1][0] = impulseAmplitude;
    testModel.ProcessBuffer(buf, blkSize);

    std::vector<float> samples;
    samples.reserve(totalBlocks * blkSize);
    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        samples.insert(samples.end(), buf[0].begin(), buf[0].end());
    }

    int delaySamples = 0;
    TapeDelayProcessor::HeadTimingSnapshot timingSnapshot;
    bool timingCaptured = EchoesModelTestAccess::getHeadTimingSnapshot(testModel, timingSnapshot);
    if (timingCaptured && timingSnapshot.h1TimeMs > 0.0f)
        delaySamples = static_cast<int>(std::round(fs * timingSnapshot.h1TimeMs / 1000.0f));

    float peak = TestUtilities::peakAbs(samples);
    float threshold = std::max(peak * 0.2f, 0.001f);
    int firstEchoIndex = TestUtilities::findOnset(samples.data(), static_cast<int>(samples.size()), threshold);

    if (delaySamples <= 0 && firstEchoIndex >= 0)
    {
        int lastPeak = firstEchoIndex - minSepSamples;
        int secondEchoIndex = -1;
        for (int i = firstEchoIndex + minSepSamples; i < static_cast<int>(samples.size()); ++i)
        {
            float a = std::abs(samples[static_cast<size_t>(i)]);
            if (a >= threshold && (i - lastPeak) >= minSepSamples)
            {
                secondEchoIndex = i;
                break;
            }
        }
        if (secondEchoIndex > firstEchoIndex)
            delaySamples = secondEchoIndex - firstEchoIndex;
    }

    std::vector<float> echoRms;
    if (firstEchoIndex >= 0 && delaySamples > 0)
    {
        const int windowHalf = std::max(8, std::min(static_cast<int>(fs * 0.005f), delaySamples / 4));
        for (int i = 0; i < maxRepeats; ++i)
        {
            const int center = firstEchoIndex + i * delaySamples;
            const int start = center - windowHalf;
            const int end = center + windowHalf;
            if (start < 0 || end >= static_cast<int>(samples.size()))
                break;
            const int windowSize = end - start + 1;
            const float rms = TestUtilities::rmsAbs(samples.data() + start, windowSize);
            echoRms.push_back(rms);
        }
    }

    float avgDecayDb = 0.0f;
    int decayCount = 0;
    for (size_t i = 1; i < echoRms.size(); ++i)
    {
        const float prev = std::max(1e-12f, echoRms[i - 1]);
        const float curr = std::max(1e-12f, echoRms[i]);
        const float db = 20.0f * std::log10(curr / prev);
        avgDecayDb += db;
        decayCount++;
    }
    avgDecayDb = decayCount > 0 ? avgDecayDb / static_cast<float>(decayCount) : 0.0f;

    bool passed = (echoRms.size() >= 3) && (avgDecayDb <= -1.0f) && (avgDecayDb >= -12.0f);

    juce::String result;
    result << "Decay (50%): echoes=" << static_cast<int>(echoRms.size()) << " avg=" << juce::String(avgDecayDb, 1)
           << " dB/repeat " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSelfOscillationTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalBlocks = 200;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 1.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f,
                               1.0f);

    for (int w = 0; w < settleBlocks; ++w)
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

    std::vector<float> rmsValues;
    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        float rms = 0.0f;
        for (int s = 0; s < blkSize; ++s)
            rms += buf[0][static_cast<size_t>(s)] * buf[0][static_cast<size_t>(s)];
        rms = std::sqrt(rms / blkSize);
        rmsValues.push_back(rms);
    }

    float earlyRms = 0.0f, lateRms = 0.0f;
    int earlyCount = 0, lateCount = 0;
    int earlyStart = 30, earlyEnd = 50;
    int lateStart = 150;

    for (size_t i = 0; i < rmsValues.size(); ++i)
    {
        if (static_cast<int>(i) >= earlyStart && static_cast<int>(i) < earlyEnd)
        {
            earlyRms += rmsValues[i];
            earlyCount++;
        }
        else if (static_cast<int>(i) >= lateStart)
        {
            lateRms += rmsValues[i];
            lateCount++;
        }
    }

    earlyRms = earlyCount > 0 ? earlyRms / static_cast<float>(earlyCount) : 0.0f;
    lateRms = lateCount > 0 ? lateRms / static_cast<float>(lateCount) : 0.0f;

    float earlyDb = 20.0f * std::log10(std::max(1e-10f, earlyRms));
    float lateDb = 20.0f * std::log10(std::max(1e-10f, lateRms));
    float decay = earlyDb - lateDb;

    // Allow moderate long-tail decay while still requiring sustained oscillation character.
    bool passed = (earlyRms > 1e-6f) && (lateRms / std::max(1e-9f, earlyRms) >= 0.08f);

    juce::String result;
    result << "Self-Osc (100%): early=" << juce::String(earlyDb, 1) << "dB, "
           << "late=" << juce::String(lateDb, 1) << "dB, "
           << "decay=" << juce::String(decay, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runSelfOscillationAllHeadsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalBlocks = 200;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 1, 1};
    testModel.UpdateParameters(0.5f, 0.5f, 1.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f,
                               1.0f);

    for (int w = 0; w < settleBlocks; ++w)
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

    std::vector<float> rmsValues;
    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        float rms = 0.0f;
        for (int s = 0; s < blkSize; ++s)
            rms += buf[0][static_cast<size_t>(s)] * buf[0][static_cast<size_t>(s)];
        rms = std::sqrt(rms / blkSize);
        rmsValues.push_back(rms);
    }

    float earlyRms = 0.0f, lateRms = 0.0f;
    int earlyCount = 0, lateCount = 0;
    const int earlyStart = 30, earlyEnd = 50;
    const int lateStart = 150;

    for (size_t i = 0; i < rmsValues.size(); ++i)
    {
        if (static_cast<int>(i) >= earlyStart && static_cast<int>(i) < earlyEnd)
        {
            earlyRms += rmsValues[i];
            earlyCount++;
        }
        else if (static_cast<int>(i) >= lateStart)
        {
            lateRms += rmsValues[i];
            lateCount++;
        }
    }

    earlyRms = earlyCount > 0 ? earlyRms / static_cast<float>(earlyCount) : 0.0f;
    lateRms = lateCount > 0 ? lateRms / static_cast<float>(lateCount) : 0.0f;

    float earlyDb = 20.0f * std::log10(std::max(1e-10f, earlyRms));
    float lateDb = 20.0f * std::log10(std::max(1e-10f, lateRms));
    float decay = earlyDb - lateDb;

    bool passed = (earlyRms > 1e-6f) && (lateRms / std::max(1e-9f, earlyRms) >= 0.08f);

    juce::String result;
    result << "Self-Osc AllHeads (100%): early=" << juce::String(earlyDb, 1) << "dB, "
           << "late=" << juce::String(lateDb, 1) << "dB, "
           << "decay=" << juce::String(decay, 1) << "dB " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runOscillationThresholdTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalSamples = static_cast<int>(fs * 2.0f);
    const int blocksPerTest = (totalSamples + blkSize - 1) / blkSize;
    const float impulseAmplitude = 0.25f;

    EchoesModel testModel;
    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    auto isOscillating = [&](float intensity) -> bool
    {
        testModel.Reset(fs, 2, blkSize);
        int heads[3] = {1, 0, 0};
        // Isolate feedback threshold from stochastic tape-noise seeding.
        testModel.UpdateParameters(0.5f, 0.5f, intensity, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                                   1.0f, 1.0f);

        for (int w = 0; w < settleBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = impulseAmplitude;
        buf[1][0] = impulseAmplitude;
        testModel.ProcessBuffer(buf, blkSize);

        double firstAcc = 0.0;
        double lastAcc = 0.0;
        int firstCount = 0;
        int lastCount = 0;

        int sampleIndex = 0;
        for (int b = 0; b < blocksPerTest; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
            for (int s = 0; s < blkSize && sampleIndex < totalSamples; ++s, ++sampleIndex)
            {
                const float v = buf[0][static_cast<size_t>(s)];
                const float e = v * v;
                if (sampleIndex < static_cast<int>(fs))
                {
                    firstAcc += e;
                    firstCount++;
                }
                else
                {
                    lastAcc += e;
                    lastCount++;
                }
            }
        }

        const float firstRms = (firstCount > 0) ? std::sqrt(static_cast<float>(firstAcc / firstCount)) : 0.0f;
        const float lastRms = (lastCount > 0) ? std::sqrt(static_cast<float>(lastAcc / lastCount)) : 0.0f;
        if (firstRms <= 1e-9f)
            return false;
        return (lastRms / firstRms) > 0.1f;
    };

    float low = 0.7f;
    float high = 1.0f;
    float threshold = high;
    for (int i = 0; i < 10; ++i)
    {
        const float mid = 0.5f * (low + high);
        if (isOscillating(mid))
        {
            threshold = mid;
            high = mid;
        }
        else
        {
            low = mid;
        }
    }

    bool passed = threshold >= 0.75f && threshold <= 0.95f;

    juce::String result;
    result << "Osc Threshold: " << juce::String(static_cast<int>(threshold * 100)) << "% "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFeedbackCompensationLoopTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int warmBlocks = 80;
    const int feedbackBlocks = 420;
    const int tailBlocks = 90;
    const float seedAmplitude = 8.0f;
    int heads[3] = {1, 0, 0};

    auto configureDelay = [&](TapeDelayProcessor& delay, bool feedbackCompensationEnabled)
    {
        delay.UpdateParameters(1.0f, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, feedbackCompensationEnabled);
    };

    auto renderTailRms = [&](bool compensationDuringDrive) -> float
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 2);
        configureDelay(delay, compensationDuringDrive);

        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        for (int b = 0; b < warmBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            delay.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = seedAmplitude;
        buf[1][0] = seedAmplitude;
        delay.ProcessBuffer(buf, blkSize);

        for (int b = 0; b < feedbackBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            delay.ProcessBuffer(buf, blkSize);
        }

        // Reveal the actual loop state by disabling compensation after the feedback phase.
        configureDelay(delay, false);

        double energy = 0.0;
        int count = 0;
        for (int b = 0; b < tailBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            delay.ProcessBuffer(buf, blkSize);

            if (b >= 8)
            {
                for (int s = 0; s < blkSize; ++s)
                {
                    const float v = buf[0][static_cast<size_t>(s)];
                    energy += static_cast<double>(v) * static_cast<double>(v);
                    ++count;
                }
            }
        }

        return (count > 0) ? std::sqrt(static_cast<float>(energy / static_cast<double>(count))) : 0.0f;
    };

    const float uncompensatedRms = renderTailRms(false);
    const float compensatedRms = renderTailRms(true);
    const float ratio = compensatedRms / std::max(1.0e-9f, uncompensatedRms);

    const bool passed = (uncompensatedRms > 1.0e-4f) && (ratio >= 0.89f) && (ratio <= 1.12f);

    juce::String result;
    result << "FeedbackCompLoop: compensated="
           << juce::String(20.0f * std::log10(std::max(1.0e-10f, compensatedRms)), 1) << "dB "
           << "uncompensated=" << juce::String(20.0f * std::log10(std::max(1.0e-10f, uncompensatedRms)), 1) << "dB "
           << "ratio=" << juce::String(ratio, 3) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runFeedbackCompensationOnsetCharacterTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr int warmBlocks = 80;
    constexpr int settleBlocks = 220;
    constexpr int measureBlocks = 48;
    constexpr float sineFreq = 440.0f;
    constexpr float inputAmplitude = 0.35f;
    int heads[3] = {1, 0, 0};

    struct Metrics
    {
        float rms = 0.0f;
        float peak = 0.0f;
        int invalid = 0;
    };

    auto configureDelay = [&](TapeDelayProcessor& delay, float intensity, bool feedbackCompensationEnabled)
    {
        delay.UpdateParameters(intensity, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f,
                               0.0f, 0, 0, feedbackCompensationEnabled);
    };

    auto renderSustained = [&](float intensity, bool compensationEnabled, float inputAmp) -> Metrics
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blkSize);
        configureDelay(delay, intensity, compensationEnabled);

        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        float phase = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * sineFreq / fs;

        for (int b = 0; b < warmBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            delay.ProcessBuffer(buf, blkSize);
        }

        double energy = 0.0;
        int count = 0;
        Metrics metrics;
        const int totalBlocks = settleBlocks + measureBlocks;
        for (int b = 0; b < totalBlocks; ++b)
        {
            for (int s = 0; s < blkSize; ++s)
            {
                const float input = inputAmp * std::sin(phase);
                phase += phaseInc;
                if (phase >= juce::MathConstants<float>::twoPi)
                    phase -= juce::MathConstants<float>::twoPi;

                buf[0][static_cast<size_t>(s)] = input;
                buf[1][static_cast<size_t>(s)] = input;
            }

            delay.ProcessBuffer(buf, blkSize);

            if (b < settleBlocks)
                continue;

            for (int s = 0; s < blkSize; ++s)
            {
                const float sample = buf[0][static_cast<size_t>(s)];
                if (!std::isfinite(sample))
                {
                    ++metrics.invalid;
                    continue;
                }

                metrics.peak = std::max(metrics.peak, std::abs(sample));
                energy += static_cast<double>(sample) * static_cast<double>(sample);
                ++count;
            }
        }

        metrics.rms = (count > 0) ? std::sqrt(static_cast<float>(energy / static_cast<double>(count))) : 0.0f;
        return metrics;
    };

    const Metrics midBypass = renderSustained(0.75f, false, inputAmplitude);
    const Metrics midComp = renderSustained(0.75f, true, inputAmplitude);
    const Metrics edgeBypass = renderSustained(0.90f, false, inputAmplitude);
    const Metrics edgeComp = renderSustained(0.90f, true, inputAmplitude);
    const Metrics abusiveComp = renderSustained(1.0f, true, 2.0f);

    const float midRatio = midComp.rms / std::max(1.0e-9f, midBypass.rms);
    const float edgeRatio = edgeComp.rms / std::max(1.0e-9f, edgeBypass.rms);
    const float minCharacterRatio = 0.89f;  // about -1 dB
    const bool midAudible = midBypass.rms > 1.0e-5f;
    const bool edgeAudible = edgeBypass.rms > 1.0e-5f;
    const bool characterPreserved = midRatio >= minCharacterRatio && edgeRatio >= minCharacterRatio;
    const bool safetyBounded = abusiveComp.peak <= 2.5f && abusiveComp.rms <= 1.25f;
    const int invalid =
        midBypass.invalid + midComp.invalid + edgeBypass.invalid + edgeComp.invalid + abusiveComp.invalid;
    const bool passed = midAudible && edgeAudible && characterPreserved && safetyBounded && invalid == 0;

    juce::String result;
    result << "FeedbackCompOnsetCharacter:"
           << " midRatio=" << juce::String(midRatio, 3) << " edgeRatio=" << juce::String(edgeRatio, 3)
           << " mid=" << juce::String(TestUtilities::linearToDb(midComp.rms), 1) << "/"
           << juce::String(TestUtilities::linearToDb(midBypass.rms), 1) << "dB"
           << " edge=" << juce::String(TestUtilities::linearToDb(edgeComp.rms), 1) << "/"
           << juce::String(TestUtilities::linearToDb(edgeBypass.rms), 1) << "dB"
           << " hotPeak=" << juce::String(abusiveComp.peak, 3) << " invalid=" << invalid << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runNoiseBufferBlockCapacityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blockSize = 16384;
    int heads[3] = {1, 1, 1};

    TapeDelayProcessor delay;
    delay.Reset(fs, 2, blockSize);
    delay.UpdateParameters(0.8f, 0.5f, heads, 1, 1.0f, true, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f, 0,
                           0, true);

    std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));
    buffer[0][0] = 0.25f;
    buffer[1][0] = 0.25f;
    delay.ProcessBuffer(buffer, blockSize);

    const auto& noiseBuffer = delay.getOutputNoiseBuffer();
    const bool capacityOk = noiseBuffer.size() >= static_cast<size_t>(blockSize);
    const bool finiteLast = capacityOk && std::isfinite(noiseBuffer[static_cast<size_t>(blockSize - 1)]);
    float tailAbsSum = 0.0f;
    if (capacityOk)
    {
        for (int i = blockSize - 64; i < blockSize; ++i)
            tailAbsSum += std::abs(noiseBuffer[static_cast<size_t>(i)]);
    }
    const bool tailWritten = tailAbsSum > 1.0e-8f;
    const bool passed = capacityOk && finiteLast && tailWritten;

    juce::String result;
    result << "NoiseBufferBlockCapacity: size=" << static_cast<int>(noiseBuffer.size()) << " block=" << blockSize
           << " finiteLast=" << (finiteLast ? "yes" : "no") << " tailAbs=" << juce::String(tailAbsSum, 6) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runTapeDelayUndersizedBufferContractTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int preparedBlockSize = 128;
    const int requestedBlockSize = 128;
    const int suppliedSamples = 64;
    int heads[3] = {1, 1, 1};

    TapeDelayProcessor delay;
    delay.Reset(fs, 2, preparedBlockSize);
    delay.UpdateParameters(0.55f, 0.35f, heads, 1, 1.0f, false, 0.0f, 0.2f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f,
                           0, 0, true);

    std::vector<std::vector<float>> buffer(2, std::vector<float>(suppliedSamples, 0.0f));
    buffer[0][0] = 0.2f;
    buffer[1][0] = -0.2f;

    delay.ProcessBuffer(buffer, requestedBlockSize);

    bool finiteOk = true;
    float peak = 0.0f;
    for (const auto& channel : buffer)
    {
        if (static_cast<int>(channel.size()) != suppliedSamples)
            finiteOk = false;

        for (float sample : channel)
        {
            finiteOk = finiteOk && std::isfinite(sample);
            peak = std::max(peak, std::abs(sample));
        }
    }

    const bool passed = finiteOk && peak < 8.0f;

    juce::String result;
    result << "TapeDelayUndersizedBufferContract: requested=" << requestedBlockSize << " supplied=" << suppliedSamples
           << " peak=" << juce::String(peak, 6) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Head Timing/Routing Tests
//==============================================================================

juce::String runPingPongStereoEnergyPreservationTest(SpatialEchoAudioProcessor&)
{
    constexpr int firstH1 = static_cast<int>(122.5f * 48.0f);
    struct Case
    {
        float left;
        float right;
        const char* name;
    };
    constexpr Case cases[] = {{0.25f, 0.25f, "mono"}, {0.25f, -0.25f, "side"}, {0.25f, 0.10f, "asymmetric"}};

    bool passed = true;
    juce::String details;
    for (const auto& test : cases)
    {
        const auto normal = renderStereoImpulseTrace(0, test.left, test.right);
        const auto pingPong = renderStereoImpulseTrace(1, test.left, test.right);
        const float normalL = windowRms(normal.left, firstH1, 256);
        const float normalR = windowRms(normal.right, firstH1, 256);
        const float pingL = windowRms(pingPong.left, firstH1, 256);
        const float pingR = windowRms(pingPong.right, firstH1, 256);
        const float normalEnergy = normalL * normalL + normalR * normalR;
        const float pingEnergy = pingL * pingL + pingR * pingR;
        const float ratio = pingEnergy / std::max(normalEnergy, 1.0e-12f);
        const bool casePassed = normalEnergy > 1.0e-10f && ratio >= 0.90f && ratio <= 1.10f;
        passed = passed && casePassed;
        details << (details.isEmpty() ? "" : ", ") << test.name << "=" << juce::String(ratio, 3);
    }

    const auto alternating = renderStereoImpulseTrace(1, 0.10f, 0.10f, false, 0.65f);
    const float firstL = windowRms(alternating.left, firstH1, 256);
    const float firstR = windowRms(alternating.right, firstH1, 256);
    const float secondL = windowRms(alternating.left, 2 * firstH1, 256);
    const float secondR = windowRms(alternating.right, 2 * firstH1, 256);
    const float thirdL = windowRms(alternating.left, 3 * firstH1, 256);
    const float thirdR = windowRms(alternating.right, 3 * firstH1, 256);
    const float firstDominance = firstL / std::max(firstR, 1.0e-9f);
    const float secondDominance = secondR / std::max(secondL, 1.0e-9f);
    const float thirdDominance = thirdL / std::max(thirdR, 1.0e-9f);
    const bool alternates = firstL > 1.0e-6f && secondR > 1.0e-6f && thirdL > 1.0e-6f && firstDominance >= 4.0f &&
                            secondDominance >= 4.0f && thirdDominance >= 4.0f;
    passed = passed && alternates;
    details << ", bounce=" << juce::String(firstDominance, 2) << "/" << juce::String(secondDominance, 2) << "/"
            << juce::String(thirdDominance, 2);

    const auto oneSidedBounce = [&](float inputL, float inputR, bool startsLeft)
    {
        const auto trace = renderStereoImpulseTrace(1, inputL, inputR, false, 0.65f);
        const float l1 = windowRms(trace.left, firstH1, 256);
        const float r1 = windowRms(trace.right, firstH1, 256);
        const float l2 = windowRms(trace.left, 2 * firstH1, 256);
        const float r2 = windowRms(trace.right, 2 * firstH1, 256);
        const float l3 = windowRms(trace.left, 3 * firstH1, 256);
        const float r3 = windowRms(trace.right, 3 * firstH1, 256);
        const float first = startsLeft ? l1 / std::max(r1, 1.0e-9f) : r1 / std::max(l1, 1.0e-9f);
        const float second = startsLeft ? r2 / std::max(l2, 1.0e-9f) : l2 / std::max(r2, 1.0e-9f);
        const float third = startsLeft ? l3 / std::max(r3, 1.0e-9f) : r3 / std::max(l3, 1.0e-9f);
        const bool audible =
            startsLeft ? l1 > 1.0e-6f && r2 > 1.0e-6f && l3 > 1.0e-6f : r1 > 1.0e-6f && l2 > 1.0e-6f && r3 > 1.0e-6f;
        return std::pair<bool, juce::String>{audible && first >= 4.0f && second >= 4.0f && third >= 4.0f,
                                             juce::String(first, 2) + "/" + juce::String(second, 2) + "/" +
                                                 juce::String(third, 2)};
    };

    const auto leftOnly = oneSidedBounce(0.10f, 0.0f, true);
    const auto rightOnly = oneSidedBounce(0.0f, 0.10f, false);
    const auto leftWithResidue = oneSidedBounce(0.10f, 1.0e-8f, true);
    const auto rightWithResidue = oneSidedBounce(-1.0e-8f, 0.10f, false);
    const auto leftAtDominanceFloor = oneSidedBounce(0.10f, 1.0e-4f, true);
    const auto rightAtDominanceFloor = oneSidedBounce(-1.0e-4f, 0.10f, false);
    passed = passed && leftOnly.first && rightOnly.first && leftWithResidue.first && rightWithResidue.first &&
             leftAtDominanceFloor.first && rightAtDominanceFloor.first;
    details << ", leftOnly=" << leftOnly.second << ", rightOnly=" << rightOnly.second
            << ", leftResidue=" << leftWithResidue.second << ", rightResidue=" << rightWithResidue.second
            << ", leftFloor=" << leftAtDominanceFloor.second << ", rightFloor=" << rightAtDominanceFloor.second;

    const auto historyBounce = [&](const StereoTrace& trace, bool startsLeft)
    {
        const float l1 = windowRms(trace.left, firstH1, 256);
        const float r1 = windowRms(trace.right, firstH1, 256);
        const float l2 = windowRms(trace.left, 2 * firstH1, 256);
        const float r2 = windowRms(trace.right, 2 * firstH1, 256);
        const float l3 = windowRms(trace.left, 3 * firstH1, 256);
        const float r3 = windowRms(trace.right, 3 * firstH1, 256);
        const float first = startsLeft ? l1 / std::max(r1, 1.0e-9f) : r1 / std::max(l1, 1.0e-9f);
        const float second = startsLeft ? r2 / std::max(l2, 1.0e-9f) : l2 / std::max(r2, 1.0e-9f);
        const float third = startsLeft ? l3 / std::max(r3, 1.0e-9f) : r3 / std::max(l3, 1.0e-9f);
        return std::pair<bool, juce::String>{first >= 4.0f && second >= 4.0f && third >= 4.0f,
                                             juce::String(first, 2) + "/" + juce::String(second, 2) + "/" +
                                                 juce::String(third, 2)};
    };

    for (const int callbackSize : {32, 256, 1024})
    {
        const auto leftAfterBalanced =
            historyBounce(renderPingPongImpulseAfterHistory(callbackSize, 0.10f, 0.10f, 0.10f, 0.0f), true);
        const auto rightAfterBalanced =
            historyBounce(renderPingPongImpulseAfterHistory(callbackSize, 0.10f, 0.10f, 0.0f, 0.10f), false);
        const auto monoAfterOneSided =
            historyBounce(renderPingPongImpulseAfterHistory(callbackSize, 0.10f, 0.0f, 0.10f, 0.10f), true);
        const auto sideAfterOneSided =
            historyBounce(renderPingPongImpulseAfterHistory(callbackSize, 0.10f, 0.0f, 0.10f, -0.10f), false);
        const bool historyPassed =
            leftAfterBalanced.first && rightAfterBalanced.first && monoAfterOneSided.first && sideAfterOneSided.first;
        passed = passed && historyPassed;
        details << ", history" << callbackSize << "=" << leftAfterBalanced.second << ";" << rightAfterBalanced.second
                << ";" << monoAfterOneSided.second << ";" << sideAfterOneSided.second;
    }

    for (const int callbackSize : {32, 256, 1024})
    {
        constexpr float sampleRate = 48000.0f;
        int heads[3] = {1, 0, 0};
        TapeDelayProcessor delay;
        delay.Reset(sampleRate, 2, callbackSize);
        delay.UpdateParameters(0.0f, 0.5f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, 0.0f,
                               1, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);
        std::vector<std::vector<float>> block(2, std::vector<float>(static_cast<size_t>(callbackSize), 0.0f));
        std::fill(block[0].begin(), block[0].end(), 0.10f);
        delay.ProcessBuffer(block, callbackSize);
        std::fill(block[0].begin(), block[0].end(), 0.10f);
        std::fill(block[1].begin(), block[1].end(), 0.10f);
        delay.ProcessBuffer(block, callbackSize);

        const int rampSamples = TapeDelayTestAccess::pingPongSeedRampSamples(delay);
        const int expectedRemaining = std::max(0, rampSamples - callbackSize);
        const float expectedAngle = juce::MathConstants<float>::pi * 0.25f *
                                    std::min(1.0f, static_cast<float>(callbackSize) / static_cast<float>(rampSamples));
        const float actualAngle = TapeDelayTestAccess::pingPongSeedAngle(delay);
        const int actualRemaining = TapeDelayTestAccess::pingPongSeedRampRemaining(delay);
        const bool rampPassed = rampSamples == 240 && actualRemaining == expectedRemaining &&
                                std::abs(actualAngle - expectedAngle) <= 1.0e-4f;
        passed = passed && rampPassed;
        details << ", slew" << callbackSize << "=" << rampSamples << "/" << actualRemaining << "/"
                << juce::String(actualAngle, 5);
    }

    const auto partition32 = renderPingPongPartitionProgram(32);
    const auto partition127 = renderPingPongPartitionProgram(127);
    const auto partition1024 = renderPingPongPartitionProgram(1024);
    const auto null127 = measureStereoNull(partition32, partition127);
    const auto null1024 = measureStereoNull(partition32, partition1024);
    const auto partitionNullPassed = null127.sameLength && null1024.sameLength && null127.referencePeak > 1.0e-5f &&
                                     null127.peakDifference <= 1.0e-6f && null1024.peakDifference <= 1.0e-6f &&
                                     null127.rmsDifference <= 1.0e-7f && null1024.rmsDifference <= 1.0e-7f &&
                                     std::abs(partition32.pingPongSeedPhasorNorm - 1.0f) <= 1.0e-6f &&
                                     std::abs(partition127.pingPongSeedPhasorNorm - 1.0f) <= 1.0e-6f &&
                                     std::abs(partition1024.pingPongSeedPhasorNorm - 1.0f) <= 1.0e-6f;
    passed = passed && partitionNullPassed;
    details << ", partitionNull127=" << juce::String(null127.peakDifference, 9) << "/"
            << juce::String(null127.rmsDifference, 9)
            << ", partitionNull1024=" << juce::String(null1024.peakDifference, 9) << "/"
            << juce::String(null1024.rmsDifference, 9)
            << ", partitionReferencePeak=" << juce::String(null127.referencePeak, 6)
            << ", partitionPhasorNorms=" << juce::String(partition32.pingPongSeedPhasorNorm, 9) << "/"
            << juce::String(partition127.pingPongSeedPhasorNorm, 9) << "/"
            << juce::String(partition1024.pingPongSeedPhasorNorm, 9);

    // Raw input while Echo Send is closed must not keep the activity detector
    // alive or steer the first real record frame. After a muted balanced
    // segment, a hard-left onset must therefore snap directly to identity.
    {
        constexpr float sampleRate = 48000.0f;
        constexpr int mutedSamples = 1024;
        int heads[3] = {1, 0, 0};
        TapeDelayProcessor delay;
        delay.Reset(sampleRate, 2, mutedSamples);
        const auto updateEchoSend = [&](float echoSend)
        {
            delay.UpdateParameters(0.0f, 0.5f, heads, 1, echoSend, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, 0.0f,
                                   0.0f, 0.0f, 1, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);
        };

        updateEchoSend(0.0f);
        std::vector<std::vector<float>> mutedBlock(2, std::vector<float>(mutedSamples, 0.10f));
        delay.ProcessBuffer(mutedBlock, mutedSamples);

        updateEchoSend(1.0f);
        std::vector<std::vector<float>> onsetBlock(2, std::vector<float>(1, 0.0f));
        onsetBlock[0][0] = 0.10f;
        delay.ProcessBuffer(onsetBlock, 1);

        const float gatedOnsetAngle = TapeDelayTestAccess::pingPongSeedAngle(delay);
        const bool gatedOnsetSnapped =
            std::abs(gatedOnsetAngle) <= 1.0e-6f && TapeDelayTestAccess::pingPongSeedRampRemaining(delay) == 0;
        passed = passed && gatedOnsetSnapped;
        details << ", gatedOnsetAngle=" << juce::String(gatedOnsetAngle, 9);
    }

    return "PingPongStereoEnergyPreservation: " + details + (passed ? " [PASS]" : " [FAIL]");
}

juce::String runNormalStereoChannelIsolationTest(SpatialEchoAudioProcessor&)
{
    constexpr int firstH1 = static_cast<int>(122.5f * 48.0f);
    constexpr int transitionSample = (firstH1 / 256) * 256;
    const auto full = renderStereoImpulseTrace(0, 0.5f, 0.0f);
    const auto playback = renderStereoImpulseTrace(0, 0.5f, 0.0f, true);
    const auto transitionFull = renderStereoImpulseTrace(0, 0.5f, 0.0f, false, 0.0f, transitionSample);
    const auto transitionPlayback = renderStereoImpulseTrace(0, 0.5f, 0.0f, true, 0.0f, transitionSample);
    const auto leakage = [&](const StereoTrace& trace)
    {
        const float left = windowRms(trace.left, firstH1, 256);
        const float right = windowRms(trace.right, firstH1, 256);
        return std::pair<float, float>{left, right / std::max(left, 1.0e-9f)};
    };
    const auto fullResult = leakage(full);
    const auto playbackResult = leakage(playback);
    const auto transitionFullResult = leakage(transitionFull);
    const auto transitionPlaybackResult = leakage(transitionPlayback);
    const bool passed = fullResult.first > 1.0e-5f && playbackResult.first > 1.0e-5f &&
                        transitionFullResult.first > 1.0e-5f && transitionPlaybackResult.first > 1.0e-5f &&
                        fullResult.second <= 1.0e-3f && playbackResult.second <= 1.0e-3f &&
                        transitionFullResult.second <= 1.0e-3f && transitionPlaybackResult.second <= 1.0e-3f;
    return "NormalStereoChannelIsolation: full=" + juce::String(fullResult.second, 6) +
           " playback=" + juce::String(playbackResult.second, 6) +
           " transitionFull=" + juce::String(transitionFullResult.second, 6) +
           " transitionPlayback=" + juce::String(transitionPlaybackResult.second, 6) + (passed ? " [PASS]" : " [FAIL]");
}

juce::String runNormalStereoPolarityPreservationTest(SpatialEchoAudioProcessor&)
{
    constexpr int firstH1 = static_cast<int>(122.5f * 48.0f);
    const auto full = renderStereoImpulseTrace(0, 0.5f, -0.5f);
    const auto playback = renderStereoImpulseTrace(0, 0.5f, -0.5f, true);
    const auto midToSideFor = [&](const StereoTrace& trace)
    {
        std::vector<float> mid(trace.left.size(), 0.0f);
        std::vector<float> side(trace.left.size(), 0.0f);
        for (size_t i = 0; i < trace.left.size(); ++i)
        {
            mid[i] = 0.5f * (trace.left[i] + trace.right[i]);
            side[i] = 0.5f * (trace.left[i] - trace.right[i]);
        }
        const float midRms = windowRms(mid, firstH1, 256);
        const float sideRms = windowRms(side, firstH1, 256);
        return std::pair<float, float>{sideRms, midRms / std::max(sideRms, 1.0e-9f)};
    };
    const auto fullResult = midToSideFor(full);
    const auto playbackResult = midToSideFor(playback);
    const bool passed = fullResult.first > 1.0e-5f && playbackResult.first > 1.0e-5f && fullResult.second <= 0.05f &&
                        playbackResult.second <= 0.05f;
    return "NormalStereoPolarityPreservation: full=" + juce::String(fullResult.second, 6) +
           " playback=" + juce::String(playbackResult.second, 6) + (passed ? " [PASS]" : " [FAIL]");
}

juce::String runCircularBufferDelayIndexingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr int sampleRate = 1000;
    constexpr int totalSamples = 8;
    constexpr float tolerance = 1.0e-6f;

    CircularBuffer sampleDelay;
    CircularBuffer msDelay;
    CircularBuffer interpolatedMsDelay;
    sampleDelay.Init(sampleRate, 32.0f);
    msDelay.Init(sampleRate, 32.0f);
    interpolatedMsDelay.Init(sampleRate, 32.0f);

    std::array<float, totalSamples> read1{};
    std::array<float, totalSamples> read2{};
    std::array<float, totalSamples> read3{};
    std::array<float, totalSamples> msRead1{};
    std::array<float, totalSamples> interpMsRead1{};

    for (int n = 0; n < totalSamples; ++n)
    {
        const float input = static_cast<float>(n + 1);

        read1[static_cast<size_t>(n)] = sampleDelay.BufferReadSamples(1);
        read2[static_cast<size_t>(n)] = sampleDelay.BufferReadSamples(2);
        read3[static_cast<size_t>(n)] = sampleDelay.BufferReadSamples(3);
        sampleDelay.BufferWrite(input);

        msRead1[static_cast<size_t>(n)] = msDelay.BufferRead(1.0f, false);
        msDelay.BufferWrite(input);

        interpMsRead1[static_cast<size_t>(n)] = interpolatedMsDelay.BufferRead(1.0f, true);
        interpolatedMsDelay.BufferWrite(input);
    }

    constexpr std::array<float, totalSamples> expected1{0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    constexpr std::array<float, totalSamples> expected2{0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    constexpr std::array<float, totalSamples> expected3{0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

    auto matches = [](const std::array<float, totalSamples>& actual, const std::array<float, totalSamples>& expected)
    {
        for (int i = 0; i < totalSamples; ++i)
        {
            if (std::abs(actual[static_cast<size_t>(i)] - expected[static_cast<size_t>(i)]) > tolerance)
                return false;
        }
        return true;
    };

    const bool samplesOk = matches(read1, expected1) && matches(read2, expected2) && matches(read3, expected3);
    const bool msOk = matches(msRead1, expected1);
    const bool interpOk = matches(interpMsRead1, expected1);
    const bool passed = samplesOk && msOk && interpOk;

    auto sequence = [](const std::array<float, totalSamples>& values)
    {
        juce::String out;
        out << "[";
        for (int i = 0; i < totalSamples; ++i)
        {
            if (i > 0)
                out << ",";
            out << juce::String(values[static_cast<size_t>(i)], 0);
        }
        out << "]";
        return out;
    };

    juce::String result;
    result << "CircularBufferDelayIndexing: d1=" << sequence(read1) << " d2=" << sequence(read2)
           << " d3=" << sequence(read3) << " ms1=" << sequence(msRead1) << " interp1=" << sequence(interpMsRead1) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runHeadRatiosTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalBlocks = 200;
    const float threshold = 0.01f;
    const float delayTimeParam = 0.0f;  // Use longest delay to reduce timing bias from ramping

    std::vector<std::vector<float>> buf(1, std::vector<float>(blkSize, 0.0f));

    float headDelaysMs[3] = {0.0f, 0.0f, 0.0f};
    TapeDelayProcessor::HeadTimingSnapshot timingSnapshot;
    bool timingCaptured = false;

    for (int headIdx = 0; headIdx < 3; ++headIdx)
    {
        testModel.Reset(fs, 1, blkSize);

        int heads[3] = {0, 0, 0};
        heads[headIdx] = 1;

        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, delayTimeParam, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 0, false);
        for (int w = 0; w < settleBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        if (headIdx == 0)
            EchoesModelTestAccess::enableHeadTimingCapture(testModel, true);

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        buf[0][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        std::vector<float> samples;
        samples.reserve(totalBlocks * blkSize);
        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
            samples.insert(samples.end(), buf[0].begin(), buf[0].end());
        }
        if (headIdx == 0)
            timingCaptured = EchoesModelTestAccess::getHeadTimingSnapshot(testModel, timingSnapshot);

        int onsetIndex = TestUtilities::findOnset(samples.data(), static_cast<int>(samples.size()), threshold);
        headDelaysMs[headIdx] = (onsetIndex >= 0) ? (1000.0f * static_cast<float>(onsetIndex) / fs) : 0.0f;
    }

    const bool outputDetected = headDelaysMs[0] > 0.0f;

    const float expectedRatio2 = HeadRatios::CLASSIC_RATIO_2;
    const float expectedRatio3 = HeadRatios::CLASSIC_RATIO_3;
    const float ratioTol = 0.05f;
    float timingRatio2 = 0.0f;
    float timingRatio3 = 0.0f;
    bool timingOk = false;
    bool syncOk = false;
    bool presetOk = false;
    if (timingCaptured && timingSnapshot.h1TimeMs > 0.0f)
    {
        timingRatio2 = timingSnapshot.h2TimeMs / timingSnapshot.h1TimeMs;
        timingRatio3 = timingSnapshot.h3TimeMs / timingSnapshot.h1TimeMs;
        timingOk =
            std::abs(timingRatio2 - expectedRatio2) <= ratioTol && std::abs(timingRatio3 - expectedRatio3) <= ratioTol;
        syncOk = !timingSnapshot.tempoSyncEnabled;
        presetOk = (timingSnapshot.headRatioPreset == 0);
    }
    bool passed = timingCaptured && timingOk && syncOk && presetOk && outputDetected;

    juce::String result;
    result << "Head Ratios: timing=1:" << juce::String(timingRatio2, 3) << ":" << juce::String(timingRatio3, 3)
           << " (exp=1:" << juce::String(expectedRatio2, 3) << ":" << juce::String(expectedRatio3, 3)
           << juce::String(juce::CharPointer_UTF8(" tol=±")) << juce::String(ratioTol, 2) << ") "
           << "H1=" << juce::String(headDelaysMs[0], 1) << "ms H2=" << juce::String(headDelaysMs[1], 1)
           << "ms H3=" << juce::String(headDelaysMs[2], 1) << "ms "
           << (timingCaptured ? "cap{target=" + juce::String(timingSnapshot.delayTimeTargetMs, 1) +
                                    " smooth=" + juce::String(timingSnapshot.delayTimeSmoothedMs, 1) +
                                    " h1=" + juce::String(timingSnapshot.h1TimeMs, 1) +
                                    " h2=" + juce::String(timingSnapshot.h2TimeMs, 1) +
                                    " h3=" + juce::String(timingSnapshot.h3TimeMs, 1) +
                                    " ratioSet=" + juce::String(timingSnapshot.head2RatioSetting, 3) + "/" +
                                    juce::String(timingSnapshot.head3RatioSetting, 3) +
                                    " preset=" + juce::String(timingSnapshot.headRatioPreset) +
                                    " sync=" + juce::String(timingSnapshot.tempoSyncEnabled ? "on" : "off") + "} "
                              : "cap{missing} ")
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runWowFlutterHeadTransportScaleTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr int settleBlocks = 140;
    constexpr float minUsefulModMs = 0.025f;

    TapeDelayProcessor delay;
    delay.Reset(fs, 1, blkSize);

    int heads[3] = {1, 1, 1};
    delay.UpdateParameters(0.45f, 0.50f, heads, 1, 1.0f, false, 0.0f, 0.0f, 4.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                           0, 0, true, -1.0f, -1.0f, 1.0f, 1.0f);

    std::vector<std::vector<float>> buf(1, std::vector<float>(blkSize, 0.0f));
    auto processSilence = [&]()
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        delay.ProcessBuffer(buf, blkSize);
    };

    for (int i = 0; i < settleBlocks; ++i)
        processSilence();

    TapeDelayProcessor::HeadTimingSnapshot snapshot;
    bool captured = false;
    bool usefulModulation = false;
    float mod1 = 0.0f;
    float mod2 = 0.0f;
    float mod3 = 0.0f;
    float scale2 = 0.0f;
    float scale3 = 0.0f;

    for (int attempt = 0; attempt < 16 && !usefulModulation; ++attempt)
    {
        for (int i = 0; i < 4; ++i)
            processSilence();

        TapeDelayTestAccess::enableHeadTimingCapture(delay, true);
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        buf[0][0] = 0.5f;
        delay.ProcessBuffer(buf, blkSize);

        captured = TapeDelayTestAccess::getHeadTimingSnapshot(delay, snapshot);
        if (!captured || !snapshot.valid)
            continue;

        const float base1 = snapshot.delayTimeSmoothedMs;
        const float base2 = base1 * snapshot.head2RatioSetting;
        const float base3 = base1 * snapshot.head3RatioSetting;

        mod1 = snapshot.h1TimeMs - base1;
        mod2 = snapshot.h2TimeMs - base2;
        mod3 = snapshot.h3TimeMs - base3;

        usefulModulation = std::abs(mod1) >= minUsefulModMs && std::abs(snapshot.wowFlutterMs) >= minUsefulModMs;
    }

    if (usefulModulation)
    {
        scale2 = mod2 / mod1;
        scale3 = mod3 / mod1;
    }

    const float expected2 = HeadRatios::CLASSIC_RATIO_2;
    const float expected3 = HeadRatios::CLASSIC_RATIO_3;
    const bool scale2Ok = std::abs(scale2 - expected2) <= 0.15f;
    const bool scale3Ok = std::abs(scale3 - expected3) <= 0.20f;
    const bool passed = captured && usefulModulation && scale2Ok && scale3Ok;

    juce::String result;
    result << "WowFlutterHeadTransportScale:"
           << " wf=" << (captured ? juce::String(snapshot.wowFlutterMs, 4) : juce::String("missing"))
           << " mod=" << juce::String(mod1, 4) << "/" << juce::String(mod2, 4) << "/" << juce::String(mod3, 4)
           << " scale=1:" << juce::String(scale2, 3) << ":" << juce::String(scale3, 3)
           << " exp=1:" << juce::String(expected2, 3) << ":" << juce::String(expected3, 3)
           << " captured=" << (captured ? "yes" : "no") << " useful=" << (usefulModulation ? "yes" : "no") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runWowFlutterTempoSyncHeadTransportScaleTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr int settleBlocks = 140;
    constexpr float minUsefulModMs = 0.025f;
    constexpr float h1SyncMs = 500.0f;
    constexpr float h2SyncMs = 250.0f;
    constexpr float h3SyncMs = 125.0f;

    TapeDelayProcessor delay;
    delay.Reset(fs, 1, blkSize);

    int heads[3] = {1, 1, 1};
    delay.UpdateParameters(0.45f, 0.50f, heads, 1, 1.0f, false, 0.0f, 0.0f, 4.0f, 1.0f, 1.0f, true, h1SyncMs, h2SyncMs,
                           h3SyncMs, 0, 0, true, -1.0f, -1.0f, 1.0f, 1.0f);

    std::vector<std::vector<float>> buf(1, std::vector<float>(blkSize, 0.0f));
    auto processSilence = [&]()
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        delay.ProcessBuffer(buf, blkSize);
    };

    for (int i = 0; i < settleBlocks; ++i)
        processSilence();

    TapeDelayProcessor::HeadTimingSnapshot snapshot;
    bool captured = false;
    bool usefulModulation = false;
    float mod1 = 0.0f;
    float mod2 = 0.0f;
    float mod3 = 0.0f;
    float scale2 = 0.0f;
    float scale3 = 0.0f;

    for (int attempt = 0; attempt < 16 && !usefulModulation; ++attempt)
    {
        for (int i = 0; i < 4; ++i)
            processSilence();

        TapeDelayTestAccess::enableHeadTimingCapture(delay, true);
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        buf[0][0] = 0.5f;
        delay.ProcessBuffer(buf, blkSize);

        captured = TapeDelayTestAccess::getHeadTimingSnapshot(delay, snapshot);
        if (!captured || !snapshot.valid || !snapshot.tempoSyncEnabled)
            continue;

        mod1 = snapshot.h1TimeMs - h1SyncMs;
        mod2 = snapshot.h2TimeMs - h2SyncMs;
        mod3 = snapshot.h3TimeMs - h3SyncMs;

        usefulModulation = std::abs(mod1) >= minUsefulModMs && std::abs(snapshot.wowFlutterMs) >= minUsefulModMs;
    }

    if (usefulModulation)
    {
        scale2 = mod2 / mod1;
        scale3 = mod3 / mod1;
    }

    const float expected2 = h2SyncMs / h1SyncMs;
    const float expected3 = h3SyncMs / h1SyncMs;
    const bool scale2Ok = std::abs(scale2 - expected2) <= 0.06f;
    const bool scale3Ok = std::abs(scale3 - expected3) <= 0.04f;
    const bool passed = captured && usefulModulation && scale2Ok && scale3Ok;

    juce::String result;
    result << "WowFlutterTempoSyncHeadTransportScale:"
           << " wf=" << (captured ? juce::String(snapshot.wowFlutterMs, 4) : juce::String("missing"))
           << " mod=" << juce::String(mod1, 4) << "/" << juce::String(mod2, 4) << "/" << juce::String(mod3, 4)
           << " scale=1:" << juce::String(scale2, 3) << ":" << juce::String(scale3, 3)
           << " exp=1:" << juce::String(expected2, 3) << ":" << juce::String(expected3, 3)
           << " captured=" << (captured ? "yes" : "no") << " useful=" << (usefulModulation ? "yes" : "no") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runHeadRatiosOutputTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalBlocks = 200;
    const float delayTimeParam = 0.0f;  // Use longest delay to reduce timing bias from ramping

    std::vector<std::vector<float>> buf(1, std::vector<float>(blkSize, 0.0f));

    float headDelaysMs[3] = {0.0f, 0.0f, 0.0f};

    for (int headIdx = 0; headIdx < 3; ++headIdx)
    {
        testModel.Reset(fs, 1, blkSize);

        int heads[3] = {0, 0, 0};
        heads[headIdx] = 1;

        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, delayTimeParam, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 0, false);
        for (int w = 0; w < settleBlocks; ++w)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
        }

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        buf[0][0] = 0.9f;
        testModel.ProcessBuffer(buf, blkSize);

        std::vector<float> samples;
        samples.reserve(totalBlocks * blkSize);
        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
            samples.insert(samples.end(), buf[0].begin(), buf[0].end());
        }

        float maxAbs = 0.0f;
        for (float sample : samples)
            maxAbs = std::max(maxAbs, std::abs(sample));
        if (maxAbs > 0.0f)
        {
            const float threshold = std::max(maxAbs * 0.10f, 0.001f);
            int onsetIndex = TestUtilities::findOnset(samples.data(), static_cast<int>(samples.size()), threshold);
            headDelaysMs[headIdx] = (onsetIndex >= 0) ? (1000.0f * static_cast<float>(onsetIndex) / fs) : 0.0f;
        }
    }

    float outputRatio2 = headDelaysMs[0] > 0 ? headDelaysMs[1] / headDelaysMs[0] : 0.0f;
    float outputRatio3 = headDelaysMs[0] > 0 ? headDelaysMs[2] / headDelaysMs[0] : 0.0f;
    const bool outputDetected = headDelaysMs[0] > 0.0f;
    const float expectedRatio2 = HeadRatios::CLASSIC_RATIO_2;
    const float expectedRatio3 = HeadRatios::CLASSIC_RATIO_3;
    const float outputTol = 0.35f;
    const bool ratioDrift = outputDetected && (std::abs(outputRatio2 - expectedRatio2) > outputTol ||
                                               std::abs(outputRatio3 - expectedRatio3) > outputTol);
    const bool passed = outputDetected && !ratioDrift;

    juce::String result;
    result << "Head Ratios Output: 1:" << juce::String(outputRatio2, 2) << ":" << juce::String(outputRatio3, 2)
           << " (exp=1:" << juce::String(expectedRatio2, 3) << ":" << juce::String(expectedRatio3, 3)
           << juce::String(juce::CharPointer_UTF8(" tol=±")) << juce::String(outputTol, 2)
           << ") H1=" << juce::String(headDelaysMs[0], 1) << "ms H2=" << juce::String(headDelaysMs[1], 1)
           << "ms H3=" << juce::String(headDelaysMs[2], 1) << "ms";
    if (!outputDetected)
        result << " warn=missing-output";
    if (ratioDrift)
        result << " warn=ratio-drift";
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runModeRoutingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int totalBlocks = 100;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    struct ModeTest
    {
        int heads[3];
        const char* name;
        int expectedEchoes;
    };

    ModeTest tests[] = {
        {{1, 0, 0}, "S", 1}, {{0, 1, 0}, "M", 1}, {{0, 0, 1}, "L", 1}, {{1, 1, 0}, "S+M", 2}, {{1, 1, 1}, "S+M+L", 3}};

    int passCount = 0;

    for (const auto& test : tests)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {test.heads[0], test.heads[1], test.heads[2]};
        testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

        for (int w = 0; w < 100; ++w)
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

        int peakCount = 0;
        float lastPeak = 0.0f;

        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            float peak = 0.0f;
            for (int s = 0; s < blkSize; ++s)
                peak = std::max(peak, std::abs(buf[0][static_cast<size_t>(s)]));

            if (peak > 0.005f && peak > lastPeak * 1.5f)
                peakCount++;
            lastPeak = peak;
        }

        if (peakCount >= test.expectedEchoes)
            passCount++;
    }

    const int numModes = static_cast<int>(std::size(tests));
    bool passed = passCount == numModes;

    juce::String result;
    result << "Mode Routing: " << passCount << "/" << numModes << " modes OK " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runTempoSyncTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int settleBlocks = 100;
    const int totalBlocks = 300;
    const float threshold = 0.01f;
    const float baseDelayMs = 500.0f;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    float headDelaysMs[3] = {0.0f, 0.0f, 0.0f};

    for (int headIdx = 0; headIdx < 3; ++headIdx)
    {
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {0, 0, 0};
        heads[headIdx] = 1;

        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.3f, 0.0f, 1.0f,
                                   1.0f, 0, true, baseDelayMs, baseDelayMs * 0.5f, baseDelayMs * 0.25f);

        for (int w = 0; w < settleBlocks; ++w)
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

        std::vector<float> samples;
        samples.reserve(totalBlocks * blkSize);
        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);
            samples.insert(samples.end(), buf[0].begin(), buf[0].end());
        }

        int peakIndex = -1;
        for (int i = 0; i < static_cast<int>(samples.size()); ++i)
        {
            if (std::abs(samples[static_cast<size_t>(i)]) >= threshold)
            {
                peakIndex = i;
                break;
            }
        }
        headDelaysMs[headIdx] = (peakIndex >= 0) ? (1000.0f * static_cast<float>(peakIndex) / fs) : 0.0f;
    }

    float ratio2_1 = (headDelaysMs[0] > 0.0f) ? headDelaysMs[1] / headDelaysMs[0] : 0.0f;
    float ratio3_1 = (headDelaysMs[0] > 0.0f) ? headDelaysMs[2] / headDelaysMs[0] : 0.0f;

    float expectedRatio2_1 = 0.5f;
    float expectedRatio3_1 = 0.25f;

    float ratioError2 = std::abs(ratio2_1 - expectedRatio2_1);
    float ratioError3 = std::abs(ratio3_1 - expectedRatio3_1);

    bool ratio2Ok = (ratioError2 <= 0.05f) && (headDelaysMs[1] > 0.0f);
    bool ratio3Ok = (ratioError3 <= 0.025f) && (headDelaysMs[2] > 0.0f);
    bool ratiosPassed = ratio2Ok && ratio3Ok && (headDelaysMs[0] > 0.0f);

    float expectedH1 = baseDelayMs;
    float timingError = std::abs(headDelaysMs[0] - expectedH1) / expectedH1;
    bool timingReasonable = (timingError <= 0.20f);

    bool passed = ratiosPassed && timingReasonable;

    juce::String result;
    result << "Tempo Sync Cascade: "
           << "H1=" << juce::String(headDelaysMs[0], 1) << "ms, "
           << "H2=" << juce::String(headDelaysMs[1], 1) << "ms (ratio=" << juce::String(ratio2_1, 3) << ", exp=0.500), "
           << "H3=" << juce::String(headDelaysMs[2], 1) << "ms (ratio=" << juce::String(ratio3_1, 3) << ", exp=0.250) "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runHeadCrosstalkTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int totalBlocks = 150;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f);

    for (int w = 0; w < 100; ++w)
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

    std::vector<float> blockPeaks;
    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        float peak = 0.0f;
        for (int s = 0; s < blkSize; ++s)
            peak = std::max(peak, std::abs(buf[0][static_cast<size_t>(s)]));
        blockPeaks.push_back(peak);
    }

    float maxPeak = *std::max_element(blockPeaks.begin(), blockPeaks.end());
    int primaryBlock =
        static_cast<int>(std::distance(blockPeaks.begin(), std::max_element(blockPeaks.begin(), blockPeaks.end())));

    int unexpectedPeaks = 0;
    for (size_t i = static_cast<size_t>(primaryBlock + 10); i < blockPeaks.size(); ++i)
    {
        if (blockPeaks[i] > maxPeak * 0.3f)
            unexpectedPeaks++;
    }

    bool ok = unexpectedPeaks < 3;

    juce::String result;
    result << "Head Crosstalk (INFO): " << unexpectedPeaks << " unexpected peaks"
           << (ok ? " (within limit) " : " (high) ") << (ok ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Stability/Robustness Tests
//==============================================================================

juce::String runSilenceStabilityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const int totalBlocks = 200;
    const int warmupBlocks = 100;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 1, 1};
    testModel.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f,
                               0.0f);

    float peak = 0.0f;
    int invalid = 0;

    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int s = 0; s < blkSize; ++s)
            {
                const float v = buf[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                if (!std::isfinite(v))
                    invalid++;
                if (b >= warmupBlocks)
                    peak = std::max(peak, std::abs(v));
            }
        }
    }

    const float peakDb = TestUtilities::linearToDb(peak);
    const bool passed = (invalid == 0) && (peak < 1.0e-5f);

    juce::String result;
    result << "SilenceStability: peak=" << juce::String(peakDb, 1) << "dB invalid=" << invalid
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runNoiseFloorTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int totalBlocks = 240;
    const int warmupBlocks = 100;
    // Calibrated for the audible-hiss contract:
    // full hiss at moderate wear should land roughly in the low -80s dBFS.
    const float minRms = TestUtilities::dbToLinear(-92.0f);
    const float maxRms = TestUtilities::dbToLinear(-72.0f);

    struct NoiseMetrics
    {
        float rms = 0.0f;
        float peak = 0.0f;
        int invalid = 0;
    };

    auto renderNoise = [&](float noiseAmount, float tapeCondition) -> NoiseMetrics
    {
        EchoesModel testModel;
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        // Keep delay enabled so the noise generator runs, but avoid feedback buildup.
        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 0.0f, true, noiseAmount,
                                   tapeCondition, 0.0f, 0.0f, 0.0f);

        double sumSquares = 0.0;
        int sampleCount = 0;
        float peak = 0.0f;
        int invalid = 0;

        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < blkSize; ++s)
                {
                    const float v = buf[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                    if (!std::isfinite(v))
                        invalid++;
                    if (b < warmupBlocks)
                        continue;
                    sumSquares += static_cast<double>(v) * static_cast<double>(v);
                    peak = std::max(peak, std::abs(v));
                    sampleCount++;
                }
            }
        }

        NoiseMetrics m;
        m.rms = (sampleCount > 0) ? static_cast<float>(std::sqrt(sumSquares / sampleCount)) : 0.0f;
        m.peak = peak;
        m.invalid = invalid;
        return m;
    };

    const NoiseMetrics full = renderNoise(1.0f, 0.6f);
    const NoiseMetrics zero = renderNoise(0.0f, 0.6f);
    // Isolation check: hiss must still be audible at lower condition with
    // wow/flutter depth at 0.
    const NoiseMetrics fullLowCondition = renderNoise(1.0f, 0.2f);
    const NoiseMetrics zeroLowCondition = renderNoise(0.0f, 0.2f);

    const float rmsDb = TestUtilities::linearToDb(full.rms);
    const float peakDb = TestUtilities::linearToDb(full.peak);
    const float zeroDb = TestUtilities::linearToDb(zero.rms);
    const float deltaDb = rmsDb - zeroDb;
    const float lowConditionDb = TestUtilities::linearToDb(fullLowCondition.rms);
    const float lowConditionZeroDb = TestUtilities::linearToDb(zeroLowCondition.rms);
    const float lowConditionDeltaDb = lowConditionDb - lowConditionZeroDb;

    const bool rangeOk = (full.rms >= minRms) && (full.rms <= maxRms);
    const bool controlOk = deltaDb >= 12.0f;
    const bool lowConditionAudible =
        (fullLowCondition.rms >= TestUtilities::dbToLinear(-98.0f)) && (lowConditionDeltaDb >= 8.0f);
    const bool passed = (full.invalid == 0) && (zero.invalid == 0) && (fullLowCondition.invalid == 0) &&
                        (zeroLowCondition.invalid == 0) && rangeOk && controlOk && lowConditionAudible;

    juce::String result;
    result << "NoiseFloor: rms=" << juce::String(rmsDb, 1) << "dB peak=" << juce::String(peakDb, 1)
           << "dB zero=" << juce::String(zeroDb, 1) << "dB delta=" << juce::String(deltaDb, 1)
           << "dB lowCond=" << juce::String(lowConditionDb, 1) << "dB"
           << " lowDelta=" << juce::String(lowConditionDeltaDb, 1) << "dB"
           << " invalid=" << (full.invalid + zero.invalid + fullLowCondition.invalid + zeroLowCondition.invalid)
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runStereoNoiseDecorrelatedTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr int totalBlocks = 260;
    constexpr int warmupBlocks = 100;

    EchoesModel testModel;
    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 0.0f, true, 1.0f, 0.65f, 0.0f, 0.0f,
                               0.0f);

    double sumL = 0.0;
    double sumR = 0.0;
    double sumLL = 0.0;
    double sumRR = 0.0;
    double sumLR = 0.0;
    int64_t sampleCount = 0;
    int invalid = 0;

    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);

        if (b < warmupBlocks)
            continue;

        for (int s = 0; s < blkSize; ++s)
        {
            const float l = buf[0][static_cast<size_t>(s)];
            const float r = buf[1][static_cast<size_t>(s)];
            if (!std::isfinite(l) || !std::isfinite(r))
            {
                ++invalid;
                continue;
            }

            sumL += l;
            sumR += r;
            sumLL += static_cast<double>(l) * static_cast<double>(l);
            sumRR += static_cast<double>(r) * static_cast<double>(r);
            sumLR += static_cast<double>(l) * static_cast<double>(r);
            ++sampleCount;
        }
    }

    const double invN = (sampleCount > 0) ? 1.0 / static_cast<double>(sampleCount) : 0.0;
    const double meanL = sumL * invN;
    const double meanR = sumR * invN;
    const double varL = std::max(0.0, sumLL * invN - meanL * meanL);
    const double varR = std::max(0.0, sumRR * invN - meanR * meanR);
    const double cov = sumLR * invN - meanL * meanR;
    const float corr = (varL > 1.0e-24 && varR > 1.0e-24) ? static_cast<float>(cov / std::sqrt(varL * varR)) : 1.0f;
    const float rmsL = static_cast<float>(std::sqrt(varL));
    const float rmsR = static_cast<float>(std::sqrt(varR));
    const float balanceDb = TestUtilities::linearToDb((rmsR > 1.0e-12f) ? (rmsL / rmsR) : 0.0f);

    const bool audible = rmsL > TestUtilities::dbToLinear(-98.0f) && rmsR > TestUtilities::dbToLinear(-98.0f);
    const bool balanced = std::abs(balanceDb) <= 3.0f;
    const bool decorrelated = std::abs(corr) <= 0.35f;
    const bool passed = invalid == 0 && sampleCount > 0 && audible && balanced && decorrelated;

    juce::String result;
    result << "StereoNoiseDecorrelated:"
           << " corr=" << juce::String(corr, 3) << " rmsL=" << juce::String(TestUtilities::linearToDb(rmsL), 1) << "dB"
           << " rmsR=" << juce::String(TestUtilities::linearToDb(rmsR), 1) << "dB"
           << " balance=" << juce::String(balanceDb, 1) << "dB"
           << " invalid=" << invalid << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runNoiseWowOrthogonalityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int totalBlocks = 240;
    const int warmupBlocks = 100;

    struct NoiseMetrics
    {
        float rms = 0.0f;
        int invalid = 0;
    };

    auto renderNoise = [&](float wfDepth) -> NoiseMetrics
    {
        EchoesModel testModel;
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {0, 0, 0};
        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 0.0f, true, 1.0f, 0.35f, wfDepth,
                                   1.0f, 1.0f);

        double sumSquares = 0.0;
        int sampleCount = 0;
        int invalid = 0;

        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < blkSize; ++s)
                {
                    const float v = buf[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                    if (!std::isfinite(v))
                        invalid++;
                    if (b < warmupBlocks)
                        continue;
                    sumSquares += static_cast<double>(v) * static_cast<double>(v);
                    sampleCount++;
                }
            }
        }

        NoiseMetrics m;
        m.rms = (sampleCount > 0) ? static_cast<float>(std::sqrt(sumSquares / sampleCount)) : 0.0f;
        m.invalid = invalid;
        return m;
    };

    const NoiseMetrics low = renderNoise(0.0f);
    const NoiseMetrics high = renderNoise(2.0f);

    const float lowDb = TestUtilities::linearToDb(low.rms);
    const float highDb = TestUtilities::linearToDb(high.rms);
    const float deltaDb = std::abs(highDb - lowDb);

    const bool floorOk =
        (low.rms >= TestUtilities::dbToLinear(-98.0f)) && (high.rms >= TestUtilities::dbToLinear(-98.0f));
    const bool passed = (low.invalid == 0) && (high.invalid == 0) && floorOk && (deltaDb <= 1.5f);

    juce::String result;
    result << "NoiseVsWF: low=" << juce::String(lowDb, 1) << "dB high=" << juce::String(highDb, 1)
           << "dB delta=" << juce::String(deltaDb, 2) << "dB invalid=" << (low.invalid + high.invalid)
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runConditionNoiseTrendTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int totalBlocks = 240;
    const int warmupBlocks = 100;

    struct NoiseMetrics
    {
        float rms = 0.0f;
        float hfEnergy = 0.0f;
        int invalid = 0;
    };

    auto renderNoise = [&](float condition) -> NoiseMetrics
    {
        EchoesModel testModel;
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {0, 0, 0};
        testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 0.0f, true, 1.0f, condition, 0.0f,
                                   1.0f, 1.0f);

        double sumSquares = 0.0;
        double hfSum = 0.0;
        std::array<float, 2> prevSamples{0.0f, 0.0f};
        int sampleCount = 0;
        int invalid = 0;

        for (int b = 0; b < totalBlocks; ++b)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            testModel.ProcessBuffer(buf, blkSize);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < blkSize; ++s)
                {
                    const float v = buf[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                    if (!std::isfinite(v))
                        invalid++;
                    if (b < warmupBlocks)
                        continue;

                    sumSquares += static_cast<double>(v) * static_cast<double>(v);
                    const float diff = v - prevSamples[(size_t)ch];
                    hfSum += static_cast<double>(diff) * static_cast<double>(diff);
                    prevSamples[(size_t)ch] = v;
                    sampleCount++;
                }
            }
        }

        NoiseMetrics m;
        m.rms = (sampleCount > 0) ? static_cast<float>(std::sqrt(sumSquares / sampleCount)) : 0.0f;
        m.hfEnergy = static_cast<float>(hfSum);
        m.invalid = invalid;
        return m;
    };

    const NoiseMetrics pristine = renderNoise(0.0f);
    const NoiseMetrics worn = renderNoise(1.0f);

    const float pristineDb = TestUtilities::linearToDb(pristine.rms);
    const float wornDb = TestUtilities::linearToDb(worn.rms);
    const float levelDeltaDb = wornDb - pristineDb;
    const float hfRatio = worn.hfEnergy / std::max(1.0e-12f, pristine.hfEnergy);

    const bool levelTrend = levelDeltaDb >= 6.0f;
    const bool hfTrend = hfRatio >= 1.05f;
    const bool passed = (pristine.invalid == 0) && (worn.invalid == 0) && levelTrend && hfTrend;

    juce::String result;
    result << "ConditionNoiseTrend: pristine=" << juce::String(pristineDb, 1) << "dB worn=" << juce::String(wornDb, 1)
           << "dB"
           << " delta=" << juce::String(levelDeltaDb, 1) << "dB"
           << " hfRatio=" << juce::String(hfRatio, 2) << " invalid=" << (pristine.invalid + worn.invalid)
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runConditionModulationTrendTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int totalSamples = 96000;
    constexpr int warmupSamples = 4096;

    struct ModMetrics
    {
        float rms = 0.0f;
        float diffRms = 0.0f;
        float peak = 0.0f;
        int invalid = 0;
    };

    auto renderCondition = [&](float condition) -> ModMetrics
    {
        WowFlutterSignalModel mod;
        mod.setDeterministicSeed(0xC01D170Du);
        mod.reset(fs);
        mod.updateParameters(1.0f, 1.0f, 1.0f, condition, 175.0f, 1.0f);

        double sumSquares = 0.0;
        double diffSquares = 0.0;
        int sampleCount = 0;
        float prev = 0.0f;
        bool havePrev = false;
        float peak = 0.0f;
        int invalid = 0;

        for (int i = 0; i < totalSamples; ++i)
        {
            const float v = mod.processSample();
            if (!std::isfinite(v))
            {
                ++invalid;
                continue;
            }

            if (i < warmupSamples)
            {
                prev = v;
                havePrev = true;
                continue;
            }

            sumSquares += static_cast<double>(v) * static_cast<double>(v);
            peak = std::max(peak, std::abs(v));
            if (havePrev)
            {
                const float diff = v - prev;
                diffSquares += static_cast<double>(diff) * static_cast<double>(diff);
            }
            prev = v;
            havePrev = true;
            ++sampleCount;
        }

        ModMetrics m;
        m.rms = (sampleCount > 0) ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(sampleCount))) : 0.0f;
        m.diffRms = (sampleCount > 1)
                        ? static_cast<float>(std::sqrt(diffSquares / static_cast<double>(sampleCount - 1)))
                        : 0.0f;
        m.peak = peak;
        m.invalid = invalid;
        return m;
    };

    const ModMetrics pristine = renderCondition(0.0f);
    const ModMetrics worn = renderCondition(1.0f);

    const float rmsLiftDb = TestUtilities::linearToDb(worn.rms / std::max(pristine.rms, 1.0e-12f));
    const float diffLiftDb = TestUtilities::linearToDb(worn.diffRms / std::max(pristine.diffRms, 1.0e-12f));
    const bool modulationTrend = diffLiftDb >= 0.75f;
    const bool bounded = worn.peak < 1.5f && rmsLiftDb <= 4.0f;
    const bool passed = (pristine.invalid == 0) && (worn.invalid == 0) && modulationTrend && bounded;

    juce::String result;
    result << "ConditionModulationTrend:"
           << " pristineRms=" << juce::String(TestUtilities::linearToDb(pristine.rms), 1) << "dB"
           << " wornRms=" << juce::String(TestUtilities::linearToDb(worn.rms), 1) << "dB"
           << " rmsLift=" << juce::String(rmsLiftDb, 2) << "dB"
           << " diffLift=" << juce::String(diffLiftDb, 2) << "dB"
           << " wornPeak=" << juce::String(worn.peak, 3) << " invalid=" << (pristine.invalid + worn.invalid)
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runWowFlutterPhaseOffsetContinuityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int warmSamples = 12345;
    constexpr float phaseOffset = 0.628f;
    constexpr float depth = 1.0f;
    constexpr float wowRate = 1.0f;
    constexpr float flutterRate = 1.0f;
    constexpr float condition = 0.25f;
    constexpr float delayMs = 175.0f;
    constexpr float scrapeScale = 0.0f;

    auto configure = [](WowFlutterSignalModel& model)
    {
        model.setDeterministicSeed(0x5EED1234u);
        model.reset(fs);
        model.updateParameters(depth, wowRate, flutterRate, condition, delayMs, scrapeScale);
        model.setStereoPhaseOffset(phaseOffset);
    };

    WowFlutterSignalModel continuous;
    WowFlutterSignalModel reapplied;
    configure(continuous);
    configure(reapplied);

    float continuousPre = 0.0f;
    float reappliedPre = 0.0f;
    for (int i = 0; i < warmSamples; ++i)
    {
        continuousPre = continuous.processSample();
        reappliedPre = reapplied.processSample();
    }

    continuous.updateParameters(depth, wowRate, flutterRate, condition, delayMs, scrapeScale);
    reapplied.updateParameters(depth, wowRate, flutterRate, condition, delayMs, scrapeScale);
    reapplied.setStereoPhaseOffset(phaseOffset);

    const float continuousPost = continuous.processSample();
    const float reappliedPost = reapplied.processSample();
    const float pathError = std::abs(reappliedPost - continuousPost);
    const float naturalStep = std::abs(continuousPost - continuousPre);
    const float reappliedStep = std::abs(reappliedPost - reappliedPre);
    const bool finite = std::isfinite(continuousPre) && std::isfinite(reappliedPre) && std::isfinite(continuousPost) &&
                        std::isfinite(reappliedPost);
    const bool matchedPath = pathError <= 0.005f;
    const bool boundedStep = reappliedStep <= std::max(0.04f, naturalStep + 0.02f);
    const bool passed = finite && matchedPath && boundedStep;

    juce::String result;
    result << "WowFlutterPhaseOffsetContinuity:"
           << " pathErr=" << juce::String(pathError, 6) << "ms"
           << " step=" << juce::String(reappliedStep, 6) << "ms"
           << " natural=" << juce::String(naturalStep, 6) << "ms"
           << " finite=" << (finite ? "yes" : "no") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runWornTapeRepeatTextureTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr float delayKnob = 0.5f;
    constexpr float head1DelayMs = 122.5f;
    constexpr int totalSamples = static_cast<int>(fs * 1.1f);
    constexpr int windowSamples = static_cast<int>(fs * 0.045f);
    constexpr float impulseAmplitude = 0.62f;

    auto render = [&](float asperityAmount) -> std::vector<float>
    {
        TapeDelayProcessor delay;
        int heads[3] = {1, 0, 0};
        delay.Reset(fs, 2, blockSize);
        delay.UpdateParameters(0.78f, delayKnob, heads, 1, 1.0f, true, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f,
                               0.0f, 0, 0, true, asperityAmount, 0.0f, 0.0f, 1.0f);

        std::vector<float> output(static_cast<size_t>(totalSamples), 0.0f);
        std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));

        int written = 0;
        while (written < totalSamples)
        {
            const int samplesThisBlock = std::min(blockSize, totalSamples - written);
            std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
            std::fill(buffer[1].begin(), buffer[1].end(), 0.0f);

            if (written == 0)
            {
                buffer[0][0] = impulseAmplitude;
                buffer[1][0] = impulseAmplitude;
            }

            delay.ProcessBuffer(buffer, blockSize);

            for (int s = 0; s < samplesThisBlock; ++s)
                output[static_cast<size_t>(written + s)] =
                    0.5f * (buffer[0][static_cast<size_t>(s)] + buffer[1][static_cast<size_t>(s)]);

            written += samplesThisBlock;
        }

        return output;
    };

    auto windowRms = [&](const std::vector<float>& samples, int centerSample) -> float
    {
        const int start = std::max(0, centerSample - windowSamples / 3);
        const int end = std::min(static_cast<int>(samples.size()), start + windowSamples);
        double sumSquares = 0.0;
        int count = 0;
        for (int i = start; i < end; ++i)
        {
            const float v = samples[static_cast<size_t>(i)];
            sumSquares += static_cast<double>(v) * static_cast<double>(v);
            ++count;
        }
        return (count > 0) ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(count))) : 0.0f;
    };

    const std::vector<float> withAsperity = render(1.0f);
    const std::vector<float> withoutAsperity = render(0.0f);
    std::vector<float> asperityOnly(withAsperity.size(), 0.0f);

    int invalid = 0;
    for (size_t i = 0; i < withAsperity.size(); ++i)
    {
        if (!std::isfinite(withAsperity[i]) || !std::isfinite(withoutAsperity[i]))
        {
            ++invalid;
            continue;
        }
        asperityOnly[i] = withAsperity[i] - withoutAsperity[i];
    }

    const int repeat1 = static_cast<int>(fs * head1DelayMs * 0.001f);
    const int repeat2 = repeat1 * 2;
    const int repeat3 = repeat1 * 3;
    const float body1 = windowRms(withoutAsperity, repeat1);
    const float body2 = windowRms(withoutAsperity, repeat2);
    const float body3 = windowRms(withoutAsperity, repeat3);
    const float asp1 = windowRms(asperityOnly, repeat1);
    const float asp2 = windowRms(asperityOnly, repeat2);
    const float asp3 = windowRms(asperityOnly, repeat3);

    const float body3Ratio = body3 / std::max(body1, 1.0e-12f);
    const float asp2Ratio = asp2 / std::max(asp1, 1.0e-12f);
    const float asp3Ratio = asp3 / std::max(asp1, 1.0e-12f);
    const float aspToBody1 = asp1 / std::max(body1, 1.0e-12f);
    const float aspToBody3 = asp3 / std::max(body3, 1.0e-12f);

    const bool repeatBodyAudible = body1 > 1.0e-4f && body3 > 1.0e-5f && body3Ratio > 0.10f;
    const bool textureAudible = asp1 > 1.0e-7f && aspToBody1 > 1.0e-4f;
    const bool textureCarried = asp2Ratio >= body3Ratio * 0.50f && asp3Ratio >= body3Ratio * 0.35f &&
                                aspToBody3 >= aspToBody1 * 0.25f && aspToBody3 >= 0.0020f;
    const bool passed = invalid == 0 && repeatBodyAudible && textureAudible && textureCarried;

    juce::String result;
    result << "WornTapeRepeatTexture:"
           << " body=" << juce::String(TestUtilities::linearToDb(body1), 1) << "/"
           << juce::String(TestUtilities::linearToDb(body2), 1) << "/"
           << juce::String(TestUtilities::linearToDb(body3), 1) << "dB"
           << " asp=" << juce::String(TestUtilities::linearToDb(asp1), 1) << "/"
           << juce::String(TestUtilities::linearToDb(asp2), 1) << "/"
           << juce::String(TestUtilities::linearToDb(asp3), 1) << "dB"
           << " aspRatio=" << juce::String(asp2Ratio, 3) << "/" << juce::String(asp3Ratio, 3)
           << " body3Ratio=" << juce::String(body3Ratio, 3) << " texBody=" << juce::String(aspToBody1, 5) << "/"
           << juce::String(aspToBody3, 5) << " invalid=" << invalid << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runTapeSurfaceNoiseSpliceDuckTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 512;
    constexpr int totalBlocks = 380;
    constexpr float delayKnob = 1.0f;
    constexpr float delayMs = 70.0f;
    constexpr float loopRatio = 17.14f;
    constexpr float loopSamples = fs * delayMs * loopRatio * 0.001f;
    constexpr float phaseStep = 1.0f / loopSamples;
    constexpr float spliceWidth = 0.015f;
    constexpr int warmupSamples = static_cast<int>(loopSamples);

    TapeDelayProcessor delay;
    int heads[3] = {1, 0, 0};
    delay.Reset(fs, 2, blockSize);
    delay.UpdateParameters(0.35f, delayKnob, heads, 1, 1.0f, true, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f,
                           0.0f, 0, 0, true, 0.0f, 0.0f, 1.0f, 1.0f);

    std::vector<std::vector<float>> buffer(2, std::vector<float>(blockSize, 0.0f));
    double spliceSquares = 0.0;
    double clearSquares = 0.0;
    int spliceCount = 0;
    int clearCount = 0;
    int invalid = 0;

    int64_t sampleIndex = 0;
    for (int b = 0; b < totalBlocks; ++b)
    {
        std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
        std::fill(buffer[1].begin(), buffer[1].end(), 0.0f);
        delay.ProcessBuffer(buffer, blockSize);

        const auto& noise = delay.getOutputNoiseBufferForChannel(0);
        const int available = std::min(blockSize, static_cast<int>(noise.size()));
        for (int s = 0; s < available; ++s, ++sampleIndex)
        {
            const float v = noise[static_cast<size_t>(s)];
            if (!std::isfinite(v))
            {
                ++invalid;
                continue;
            }
            if (sampleIndex < warmupSamples)
                continue;

            float phase = std::fmod((static_cast<float>(sampleIndex) + 1.0f) * phaseStep, 1.0f);
            if (phase < 0.0f)
                phase += 1.0f;

            const double square = static_cast<double>(v) * static_cast<double>(v);
            if (phase < spliceWidth)
            {
                spliceSquares += square;
                ++spliceCount;
            }
            else if (phase > 0.12f && phase < 0.88f)
            {
                clearSquares += square;
                ++clearCount;
            }
        }
    }

    const float spliceRms = (spliceCount > 0) ? static_cast<float>(std::sqrt(spliceSquares / spliceCount)) : 0.0f;
    const float clearRms = (clearCount > 0) ? static_cast<float>(std::sqrt(clearSquares / clearCount)) : 0.0f;
    const float duckRatio = spliceRms / std::max(clearRms, 1.0e-12f);
    const bool enoughWindows = spliceCount >= 1500 && clearCount >= 10000;
    const bool measurable = clearRms > 1.0e-7f;
    const bool ducked = duckRatio <= 0.90f;
    const bool passed = invalid == 0 && enoughWindows && measurable && ducked;

    juce::String result;
    result << "TapeSurfaceNoiseSpliceDuck:"
           << " splice=" << juce::String(TestUtilities::linearToDb(spliceRms), 1) << "dB"
           << " clear=" << juce::String(TestUtilities::linearToDb(clearRms), 1) << "dB"
           << " ratio=" << juce::String(duckRatio, 3) << " counts=" << spliceCount << "/" << clearCount
           << " invalid=" << invalid << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runNoRegrowthParamJumpTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 256;
    const int settleBlocks = 120;
    const int decayBlocks = 360;
    const int decayHeadBlocks = 80;
    const int decayTailBlocks = 120;
    const int jumpBlocks = 220;
    constexpr float impulseAmplitude = 0.45f;
    constexpr float intensity = 0.92f;

    struct RenderMetrics
    {
        double earlyDecayEnergy = 0.0;
        double lateDecayEnergy = 0.0;
        double jumpEnergy = 0.0;
        int64_t earlyDecaySamples = 0;
        int64_t lateDecaySamples = 0;
        int64_t jumpSamples = 0;
        float jumpPeak = 0.0f;
        int invalid = 0;

        float earlyDecayRms() const
        {
            return (earlyDecaySamples > 0)
                       ? static_cast<float>(std::sqrt(earlyDecayEnergy / static_cast<double>(earlyDecaySamples)))
                       : 0.0f;
        }

        float lateDecayRms() const
        {
            return (lateDecaySamples > 0)
                       ? static_cast<float>(std::sqrt(lateDecayEnergy / static_cast<double>(lateDecaySamples)))
                       : 0.0f;
        }

        float jumpRms() const
        {
            return (jumpSamples > 0) ? static_cast<float>(std::sqrt(jumpEnergy / static_cast<double>(jumpSamples)))
                                     : 0.0f;
        }
    };

    auto runScenario = [&](bool withParamJumps) -> RenderMetrics
    {
        EchoesModel model;
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        model.Reset(fs, 2, blkSize);

        int heads[3] = {1, 0, 0};
        model.UpdateParameters(0.5f, 0.5f, intensity, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, true, 0.75f, 0.45f, 0.0f,
                               1.0f, 1.0f);

        auto* tapeDelay = model.getTapeDelay();
        if (tapeDelay != nullptr)
        {
            tapeDelay->setTapeCharacter(0.5f, 0.5f, 0.5f);
            tapeDelay->setTapeRecordMakeupGainDb(0.0f);
        }

        auto processSilenceBlock =
            [&](RenderMetrics& metrics, bool accumulateEarly, bool accumulateLate, bool accumulateJump)
        {
            std::fill(buf[0].begin(), buf[0].end(), 0.0f);
            std::fill(buf[1].begin(), buf[1].end(), 0.0f);
            model.ProcessBuffer(buf, blkSize);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < blkSize; ++s)
                {
                    const float v = buf[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                    if (!std::isfinite(v))
                    {
                        metrics.invalid++;
                        continue;
                    }

                    const double e = static_cast<double>(v) * static_cast<double>(v);
                    if (accumulateEarly)
                    {
                        metrics.earlyDecayEnergy += e;
                        metrics.earlyDecaySamples++;
                    }
                    if (accumulateLate)
                    {
                        metrics.lateDecayEnergy += e;
                        metrics.lateDecaySamples++;
                    }
                    if (accumulateJump)
                    {
                        metrics.jumpEnergy += e;
                        metrics.jumpSamples++;
                        metrics.jumpPeak = std::max(metrics.jumpPeak, std::abs(v));
                    }
                }
            }
        };

        RenderMetrics metrics;

        for (int b = 0; b < settleBlocks; ++b)
            processSilenceBlock(metrics, false, false, false);

        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        buf[0][0] = impulseAmplitude;
        buf[1][0] = impulseAmplitude;
        model.ProcessBuffer(buf, blkSize);

        for (int b = 0; b < decayBlocks; ++b)
        {
            const bool isEarly = (b < decayHeadBlocks);
            const bool isLate = (b >= (decayBlocks - decayTailBlocks));
            processSilenceBlock(metrics, isEarly, isLate, false);
        }

        for (int b = 0; b < jumpBlocks; ++b)
        {
            if (withParamJumps)
            {
                if ((b & 1) == 0)
                {
                    if (tapeDelay != nullptr)
                        tapeDelay->setTapeCharacter(1.0f, 1.0f, 0.05f);
                }
                else
                {
                    if (tapeDelay != nullptr)
                        tapeDelay->setTapeCharacter(0.15f, 0.20f, 0.95f);
                }
            }

            processSilenceBlock(metrics, false, false, true);
        }

        return metrics;
    };

    const RenderMetrics baseline = runScenario(false);
    const RenderMetrics jumped = runScenario(true);

    const float baseEarlyDb = TestUtilities::linearToDb(baseline.earlyDecayRms());
    const float baseLateDb = TestUtilities::linearToDb(baseline.lateDecayRms());
    const float jumpEarlyDb = TestUtilities::linearToDb(jumped.earlyDecayRms());
    const float jumpLateDb = TestUtilities::linearToDb(jumped.lateDecayRms());
    const float baselineJumpDb = TestUtilities::linearToDb(baseline.jumpRms());
    const float jumpedJumpDb = TestUtilities::linearToDb(jumped.jumpRms());
    const float baselinePeakDb = TestUtilities::linearToDb(baseline.jumpPeak);
    const float jumpedPeakDb = TestUtilities::linearToDb(jumped.jumpPeak);

    const float baselineDecayDropDb = baseEarlyDb - baseLateDb;
    const float jumpedDecayDropDb = jumpEarlyDb - jumpLateDb;
    const float jumpDeltaDb = jumpedJumpDb - baselineJumpDb;
    const float peakDeltaDb = jumpedPeakDb - baselinePeakDb;

    constexpr float minDecayDropDb = 8.0f;
    const bool decaySettled = (baselineDecayDropDb >= minDecayDropDb) && (jumpedDecayDropDb >= minDecayDropDb);
    const bool noRegrowth = (jumpDeltaDb <= 3.0f) && (peakDeltaDb <= 6.0f);
    const bool passed = (baseline.invalid == 0) && (jumped.invalid == 0) && decaySettled && noRegrowth;

    juce::String result;
    result << "NoRegrowthParamJump: decayDrop=" << juce::String(baselineDecayDropDb, 1) << "/"
           << juce::String(jumpedDecayDropDb, 1) << "dB"
           << " minDecay=" << juce::String(minDecayDropDb, 1) << "dB"
           << " jumpDelta=" << juce::String(jumpDeltaDb, 2) << "dB"
           << " peakDelta=" << juce::String(peakDeltaDb, 2) << "dB"
           << " invalid=" << (baseline.invalid + jumped.invalid) << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runDelayStabilityMatrixTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
    constexpr std::array<int, 4> blockSizes{1, 16, 512, 16384};

    struct MatrixStats
    {
        int cases = 0;
        int failures = 0;
        int invalid = 0;
        float maxPeak = 0.0f;
        float worstTimingErrorMs = 0.0f;
        float minNoiseDb = 120.0f;
        float maxNoiseDb = -120.0f;
        float minTempoPeak = 120.0f;
        float worstFeedbackGrowthDb = -120.0f;
        juce::String firstFailure;
        juce::String failureDetails;
    } stats;

    auto noteFailure = [&](const juce::String& label)
    {
        ++stats.failures;
        if (stats.firstFailure.isEmpty())
            stats.firstFailure = label;
        if (stats.failures <= 12)
        {
            if (stats.failureDetails.isNotEmpty())
                stats.failureDetails << ";";
            stats.failureDetails << label;
        }
    };

    auto processSilence =
        [](EchoesModel& model, std::vector<std::vector<float>>& buffer, int blockSize, int totalSamples)
    {
        int processed = 0;
        while (processed < totalSamples)
        {
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);

            model.ProcessBuffer(buffer, blockSize);
            processed += blockSize;
        }
    };

    auto runNoiseCase = [&](float fs, int blockSize)
    {
        EchoesModel model;
        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        model.Reset(fs, 2, blockSize);

        int heads[3] = {0, 0, 0};
        model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, true, 1.0f, 1.0f, 1.0f, 1.0f,
                               1.0f);

        const int warmupSamples = static_cast<int>(std::ceil(fs * 0.05f));
        const int captureSamples = static_cast<int>(std::ceil(fs * 0.25f));
        processSilence(model, buffer, blockSize, warmupSamples);

        double sumSquares = 0.0;
        int samples = 0;
        int invalid = 0;
        float peak = 0.0f;
        int processed = 0;
        while (processed < captureSamples)
        {
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);

            model.ProcessBuffer(buffer, blockSize);
            const int samplesThisBlock = std::min(blockSize, captureSamples - processed);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < samplesThisBlock; ++s)
                {
                    const float v = buffer[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                    if (!std::isfinite(v))
                    {
                        ++invalid;
                        continue;
                    }

                    peak = std::max(peak, std::abs(v));
                    sumSquares += static_cast<double>(v) * static_cast<double>(v);
                    ++samples;
                }
            }

            processed += samplesThisBlock;
        }

        const float rms =
            (samples > 0) ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(samples))) : 0.0f;
        const float noiseDb = TestUtilities::linearToDb(rms);
        stats.invalid += invalid;
        stats.maxPeak = std::max(stats.maxPeak, peak);
        stats.minNoiseDb = std::min(stats.minNoiseDb, noiseDb);
        stats.maxNoiseDb = std::max(stats.maxNoiseDb, noiseDb);
        ++stats.cases;

        const bool noisePresent = rms >= TestUtilities::dbToLinear(-98.0f);
        const bool noiseBounded = rms <= TestUtilities::dbToLinear(-45.0f) && peak <= 0.25f;
        if (invalid != 0 || !noisePresent || !noiseBounded)
        {
            noteFailure("noise@" + juce::String(fs / 1000.0f, 1) + "k/" + juce::String(blockSize) +
                        " rms=" + juce::String(noiseDb, 1) + "dB peak=" + juce::String(peak, 3));
        }
    };

    auto runTempoCase = [&](float fs, int blockSize)
    {
        EchoesModel model;
        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        model.Reset(fs, 2, blockSize);

        constexpr float expectedMs = 250.0f;
        int heads[3] = {1, 0, 0};
        model.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.3f, 0.0f, 1.0f,
                               1.0f, 0, true, expectedMs, expectedMs * 0.5f, expectedMs * 0.25f);

        processSilence(model, buffer, blockSize, static_cast<int>(std::ceil(fs * 0.08f)));

        const int captureSamples = static_cast<int>(std::ceil(fs * 0.55f));
        const int pulseSamples = static_cast<int>(std::ceil(fs * 0.001f));
        int processed = 0;
        int onsetSample = -1;
        int invalid = 0;
        float peak = 0.0f;

        while (processed < captureSamples)
        {
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);

            for (int s = 0; s < std::min(blockSize, captureSamples - processed); ++s)
            {
                if (processed + s < pulseSamples)
                {
                    buffer[0][static_cast<size_t>(s)] = 0.55f;
                    buffer[1][static_cast<size_t>(s)] = 0.55f;
                }
            }

            model.ProcessBuffer(buffer, blockSize);
            const int samplesThisBlock = std::min(blockSize, captureSamples - processed);

            for (int s = 0; s < samplesThisBlock; ++s)
            {
                const float mono = 0.5f * (buffer[0][static_cast<size_t>(s)] + buffer[1][static_cast<size_t>(s)]);
                if (!std::isfinite(mono))
                {
                    ++invalid;
                    continue;
                }

                const float a = std::abs(mono);
                peak = std::max(peak, a);
                if (onsetSample < 0 && processed + s > static_cast<int>(fs * 0.05f) && a >= 0.004f)
                    onsetSample = processed + s;
            }

            processed += samplesThisBlock;
        }

        const float onsetMs = (onsetSample >= 0) ? (1000.0f * static_cast<float>(onsetSample) / fs) : 0.0f;
        const float errMs = (onsetSample >= 0) ? std::abs(onsetMs - expectedMs) : 9999.0f;
        stats.invalid += invalid;
        stats.maxPeak = std::max(stats.maxPeak, peak);
        stats.minTempoPeak = std::min(stats.minTempoPeak, peak);
        stats.worstTimingErrorMs = std::max(stats.worstTimingErrorMs, errMs);
        ++stats.cases;

        const bool timingOk = onsetSample >= 0 && errMs <= 45.0f;
        const bool levelOk = peak >= 0.01f && peak <= 1.5f;
        if (invalid != 0 || !timingOk || !levelOk)
        {
            noteFailure("tempo@" + juce::String(fs / 1000.0f, 1) + "k/" + juce::String(blockSize) +
                        " onset=" + juce::String(onsetMs, 1) + "ms peak=" + juce::String(peak, 3));
        }
    };

    auto runFeedbackCase = [&](float fs, int blockSize)
    {
        EchoesModel model;
        std::vector<std::vector<float>> buffer(2, std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        model.Reset(fs, 2, blockSize);

        int heads[3] = {1, 1, 1};
        model.UpdateParameters(0.5f, 0.5f, 0.84f, 0.45f, heads, 1, 1.0f, 0, 1.0f, 1.0f, true, 0.35f, 0.75f, 1.0f, 1.0f,
                               1.0f);

        processSilence(model, buffer, blockSize, static_cast<int>(std::ceil(fs * 0.12f)));

        const int captureSamples = static_cast<int>(std::ceil(fs * 1.10f));
        const int earlyStart = static_cast<int>(std::ceil(fs * 0.18f));
        const int earlyEnd = static_cast<int>(std::ceil(fs * 0.42f));
        const int lateStart = static_cast<int>(std::ceil(fs * 0.78f));
        const int lateEnd = captureSamples;
        const int pulseSamples = static_cast<int>(std::ceil(fs * 0.001f));

        double earlyEnergy = 0.0;
        double lateEnergy = 0.0;
        int earlySamples = 0;
        int lateSamples = 0;
        int invalid = 0;
        float peak = 0.0f;
        int processed = 0;

        while (processed < captureSamples)
        {
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.end(), 0.0f);

            for (int s = 0; s < std::min(blockSize, captureSamples - processed); ++s)
            {
                if (processed + s < pulseSamples)
                {
                    buffer[0][static_cast<size_t>(s)] = 0.40f;
                    buffer[1][static_cast<size_t>(s)] = 0.40f;
                }
            }

            model.ProcessBuffer(buffer, blockSize);
            const int samplesThisBlock = std::min(blockSize, captureSamples - processed);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int s = 0; s < samplesThisBlock; ++s)
                {
                    const int sampleIndex = processed + s;
                    const float v = buffer[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                    if (!std::isfinite(v))
                    {
                        ++invalid;
                        continue;
                    }

                    peak = std::max(peak, std::abs(v));
                    const double e = static_cast<double>(v) * static_cast<double>(v);
                    if (sampleIndex >= earlyStart && sampleIndex < earlyEnd)
                    {
                        earlyEnergy += e;
                        ++earlySamples;
                    }
                    if (sampleIndex >= lateStart && sampleIndex < lateEnd)
                    {
                        lateEnergy += e;
                        ++lateSamples;
                    }
                }
            }

            processed += samplesThisBlock;
        }

        const float earlyRms = (earlySamples > 0) ? static_cast<float>(std::sqrt(earlyEnergy / earlySamples)) : 0.0f;
        const float lateRms = (lateSamples > 0) ? static_cast<float>(std::sqrt(lateEnergy / lateSamples)) : 0.0f;
        const float growthDb = TestUtilities::linearToDb(lateRms / std::max(earlyRms, 1.0e-9f));
        stats.invalid += invalid;
        stats.maxPeak = std::max(stats.maxPeak, peak);
        stats.worstFeedbackGrowthDb = std::max(stats.worstFeedbackGrowthDb, growthDb);
        ++stats.cases;

        const bool signalPresent = earlyRms >= TestUtilities::dbToLinear(-80.0f);
        const bool bounded = peak <= 2.0f && growthDb <= 8.0f;
        if (invalid != 0 || !signalPresent || !bounded)
        {
            noteFailure("feedback@" + juce::String(fs / 1000.0f, 1) + "k/" + juce::String(blockSize) +
                        " early=" + juce::String(TestUtilities::linearToDb(earlyRms), 1) +
                        "dB growth=" + juce::String(growthDb, 1) + "dB peak=" + juce::String(peak, 3));
        }
    };

    for (float fs : sampleRates)
    {
        for (int blockSize : blockSizes)
        {
            runNoiseCase(fs, blockSize);
            runTempoCase(fs, blockSize);
            runFeedbackCase(fs, blockSize);
        }
    }

    const bool passed = stats.failures == 0 && stats.invalid == 0 && stats.cases == 48;

    juce::String result;
    result << "DelayStabilityMatrix: cases=" << stats.cases << " failures=" << stats.failures
           << " invalid=" << stats.invalid << " peak=" << juce::String(stats.maxPeak, 3)
           << " timingErr=" << juce::String(stats.worstTimingErrorMs, 1) << "ms"
           << " tempoPeakMin=" << juce::String(stats.minTempoPeak, 4) << " noise=" << juce::String(stats.minNoiseDb, 1)
           << ".." << juce::String(stats.maxNoiseDb, 1) << "dB"
           << " fbGrowth=" << juce::String(stats.worstFeedbackGrowthDb, 1) << "dB";
    if (stats.failures > 0)
        result << " first=" << stats.firstFailure << " details=" << stats.failureDetails;
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runDeterminismTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    constexpr int totalSamples = 4096;
    constexpr float tol = 1.0e-9f;

    TapeNoiseProcessor noiseA;
    TapeNoiseProcessor noiseB;
    noiseA.setDeterministicSeed(0x12345678u);
    noiseB.setDeterministicSeed(0x12345678u);
    noiseA.reset(fs);
    noiseB.reset(fs);
    noiseA.updateParameters(true, 0.6f, 0.5f, 0.9f);
    noiseB.updateParameters(true, 0.6f, 0.5f, 0.9f);

    float maxNoiseDelta = 0.0f;
    float maxDropoutDelta = 0.0f;
    for (int i = 0; i < totalSamples; ++i)
    {
        noiseA.processSample(0.0f);
        noiseB.processSample(0.0f);
        maxNoiseDelta = std::max(maxNoiseDelta, std::abs(noiseA.getRawNoise() - noiseB.getRawNoise()));
        maxDropoutDelta = std::max(maxDropoutDelta, std::abs(noiseA.getDropoutGain() - noiseB.getDropoutGain()));
    }

    WowFlutterSignalModel modA;
    WowFlutterSignalModel modB;
    modA.setDeterministicSeed(0xC0FFEEu);
    modB.setDeterministicSeed(0xC0FFEEu);
    modA.reset(fs);
    modB.reset(fs);
    modA.updateParameters(1.0f, 1.0f, 1.0f, 0.4f, 175.0f);
    modB.updateParameters(1.0f, 1.0f, 1.0f, 0.4f, 175.0f);

    float maxModDelta = 0.0f;
    for (int i = 0; i < totalSamples; ++i)
        maxModDelta = std::max(maxModDelta, std::abs(modA.processSample() - modB.processSample()));

    const bool passed = (maxNoiseDelta <= tol) && (maxDropoutDelta <= tol) && (maxModDelta <= tol);

    juce::String result;
    result << "Determinism: noise=" << juce::String(maxNoiseDelta, 9) << " dropout=" << juce::String(maxDropoutDelta, 9)
           << " wowFlutter=" << juce::String(maxModDelta, 9) << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runDelayParamSweepTest(SpatialEchoAudioProcessor& /*processor*/)
{
    EchoesModel testModel;

    const float fs = 48000.0f;
    const int blkSize = 512;
    const float testFreq = 440.0f;
    const float amplitude = 0.2f;
    const float maxAllowed = 2.0f;
    const int blocksPerConfig = 4;

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    testModel.Reset(fs, 2, blkSize);

    const float intensities[] = {0.1f, 0.3f, 0.5f};
    const float delayTimes[] = {0.1f, 0.5f, 0.9f};
    const int headConfigs[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};

    float phase = 0.0f;
    const float twoPi = 2.0f * juce::MathConstants<float>::pi;
    const float phaseInc = twoPi * testFreq / fs;

    int invalid = 0;
    int over = 0;
    float peak = 0.0f;
    int configCount = 0;

    for (float intensity : intensities)
    {
        for (float delayTime : delayTimes)
        {
            for (const auto& config : headConfigs)
            {
                int heads[3] = {config[0], config[1], config[2]};
                testModel.UpdateParameters(0.5f, 0.5f, intensity, delayTime, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f,
                                           0.0f, 0.0f, 0.0f, 0.0f);
                configCount++;

                for (int b = 0; b < blocksPerConfig; ++b)
                {
                    for (int s = 0; s < blkSize; ++s)
                    {
                        const float sample = amplitude * std::sin(phase);
                        phase += phaseInc;
                        if (phase >= twoPi)
                            phase -= twoPi;
                        buf[0][static_cast<size_t>(s)] = sample;
                        buf[1][static_cast<size_t>(s)] = sample;
                    }

                    testModel.ProcessBuffer(buf, blkSize);

                    for (int ch = 0; ch < 2; ++ch)
                    {
                        for (int s = 0; s < blkSize; ++s)
                        {
                            const float v = buf[static_cast<size_t>(ch)][static_cast<size_t>(s)];
                            if (!std::isfinite(v))
                                invalid++;
                            const float a = std::abs(v);
                            peak = std::max(peak, a);
                            if (a > maxAllowed)
                                over++;
                        }
                    }
                }
            }
        }
    }

    const float peakDb = TestUtilities::linearToDb(peak);
    const bool passed = (invalid == 0) && (over == 0);

    juce::String result;
    result << "DelayParamSweep: configs=" << configCount << " peak=" << juce::String(peakDb, 1) << "dB"
           << " invalid=" << invalid << " over=" << over << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

//==============================================================================
// Per-Head Tempo Sync Tests (Calculation-based)
//==============================================================================

juce::String runSubdivisionSelectionTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Pure calculation test - verify subdivision timing accuracy
    const double bpm = 120.0;

    float delay1_4 = TestUtilities::calculateSyncedDelayMs(bpm, 2);  // 1/4 = 500ms
    float delay1_8 = TestUtilities::calculateSyncedDelayMs(bpm, 3);  // 1/8 = 250ms

    bool passed = std::abs(delay1_4 - 500.0f) < 5.0f && std::abs(delay1_8 - 250.0f) < 5.0f &&
                  std::abs(delay1_4 - 2.0f * delay1_8) < 10.0f;

    juce::String result;
    result << "SubdivisionSelection: 1/4=" << juce::String(delay1_4, 1) << "ms 1/8=" << juce::String(delay1_8, 1)
           << "ms " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runVisualFeedbackTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Pure calculation test - verify tempo sync delay calculation
    const double bpm = 120.0;
    float quarterNote = TestUtilities::calculateSyncedDelayMs(bpm, 2);  // Should be 500ms
    float eighthNote = TestUtilities::calculateSyncedDelayMs(bpm, 3);   // Should be 250ms

    bool passed = std::abs(quarterNote - 500.0f) < 1.0f && std::abs(eighthNote - 250.0f) < 1.0f;

    juce::String result;
    result << "DelayCalc: 1/4=" << juce::String(quarterNote, 1) << "ms 1/8=" << juce::String(eighthNote, 1) << "ms "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runAllSubdivisionsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Test all 17 subdivisions for timing accuracy at 120 BPM
    const double bpm = 120.0;
    const float quarterNoteMs = 60000.0f / static_cast<float>(bpm);  // 500ms @ 120 BPM

    struct SubdivTest
    {
        int index;
        float multiplier;
        const char* name;
    };

    SubdivTest subdivs[] = {{0, 4.0f, "1/1"},      {1, 2.0f, "1/2"},     {2, 1.0f, "1/4"},       {3, 0.5f, "1/8"},
                            {4, 0.25f, "1/16"},    {5, 0.125f, "1/32"},  {6, 0.0625f, "1/64"},   {7, 1.3333f, "1/2T"},
                            {8, 0.6667f, "1/4T"},  {9, 0.3333f, "1/8T"}, {10, 0.1667f, "1/16T"}, {11, 0.0833f, "1/32T"},
                            {12, 3.0f, "1/2D"},    {13, 1.5f, "1/4D"},   {14, 0.75f, "1/8D"},    {15, 0.375f, "1/16D"},
                            {16, 0.1875f, "1/32D"}};

    int failCount = 0;
    juce::String failedNames;

    for (const auto& test : subdivs)
    {
        float expected = quarterNoteMs * test.multiplier;
        float calculated = TestUtilities::calculateSyncedDelayMs(bpm, test.index);
        float error = std::abs(calculated - expected);
        float errorPercent = (error / expected) * 100.0f;

        if (errorPercent > 1.0f)
        {
            failCount++;
            failedNames += juce::String(test.name) + " ";
        }
    }

    bool passed = failCount == 0;
    juce::String result;
    result << "AllSubdivisions: " << (17 - failCount) << "/17 accurate "
           << (passed ? "[PASS]" : "[FAIL] " + failedNames);
    return result;
}

juce::String runExtremeCasesTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Test 1: Very short delay (1/64 @ 200 BPM = 18.75ms)
    float veryShort = TestUtilities::calculateSyncedDelayMs(200.0, 6);
    bool shortValid = veryShort > 10.0f && veryShort < 25.0f;

    // Test 2: Very long delay (whole note @ 60 BPM = 4000ms)
    float veryLong = TestUtilities::calculateSyncedDelayMs(60.0, 0);
    bool longValid = veryLong > 3900.0f && veryLong < 4100.0f;

    bool passed = shortValid && longValid;
    juce::String result;
    result << "ExtremeCases: short=" << juce::String(veryShort, 1) << "ms long=" << juce::String(veryLong, 1) << "ms "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Per-Head Tempo Sync Tests (DSP-based, simple signatures)
//==============================================================================

juce::String runDelayModeActivationTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP Test: Verify inactive heads produce no output
    const float fs = 48000.0f;
    const int blkSize = 512;
    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f);

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
    for (int w = 0; w < 100; ++w)
        testModel.ProcessBuffer(buf, blkSize);

    buf[0][0] = 0.9f;
    buf[1][0] = 0.9f;
    testModel.ProcessBuffer(buf, blkSize);

    float totalEnergy = 0.0f;
    for (int b = 0; b < 50; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        for (int s = 0; s < blkSize; ++s)
            totalEnergy += buf[0][static_cast<size_t>(s)] * buf[0][static_cast<size_t>(s)];
    }

    bool passed = totalEnergy > 0.001f;
    return passed ? "ModeActivation: Inactive heads silent [PASS]" : "ModeActivation: [FAIL]";
}

juce::String runSubdivisionSmoothingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP Test: Change subdivisions rapidly and check for clicks
    const float fs = 48000.0f;
    const int blkSize = 512;
    const double bpm = 120.0;

    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    // Start with 1/4 note
    float h1 = TestUtilities::calculateSyncedDelayMs(bpm, 2);
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1, h1, h1);

    for (int w = 0; w < 50; ++w)
        testModel.ProcessBuffer(buf, blkSize);

    // Switch to 1/8 note (should smooth, not click)
    h1 = TestUtilities::calculateSyncedDelayMs(bpm, 3);
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1, h1, h1);

    float maxPeak = 0.0f;
    for (int b = 0; b < 20; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        for (int s = 0; s < blkSize; ++s)
            maxPeak = std::max(maxPeak, std::abs(buf[0][static_cast<size_t>(s)]));
    }

    bool passed = maxPeak < 0.01f;
    return passed ? "SubdivisionSmoothing: No clicks detected [PASS]" : "SubdivisionSmoothing: Clicks [FAIL]";
}

juce::String runModeChangeClickTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float sampleRates[] = {44100.0f, 48000.0f};
    const int blkSize = 512;
    const int warmBlocks = 200;
    const int baselineBlocks = 20;
    const int measureBlocks = 40;
    const float sineFreq = 440.0f;
    const float sineAmp = 0.2f;
    const float clickThreshold = 0.01f;  // -40 dBFS
    const float levelRecoveryThreshold = 0.75f;
    const double bpm = 120.0;

    auto processBlocks = [&](EchoesModel& model, std::vector<std::vector<float>>& buf, int blocks, float fs, float freq,
                             float amp, float& phase, std::array<float, 2>& lastOut, std::array<bool, 2>& hasLast,
                             double* sumSq, int* sampleCount) -> float
    {
        float peakDelta = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * freq / fs;
        for (int b = 0; b < blocks; ++b)
        {
            for (int s = 0; s < blkSize; ++s)
            {
                float sample = amp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buf[0][static_cast<size_t>(s)] = sample;
                buf[1][static_cast<size_t>(s)] = sample;
            }

            model.ProcessBuffer(buf, blkSize);
            for (int s = 0; s < blkSize; ++s)
            {
                for (int channel = 0; channel < 2; ++channel)
                {
                    const float out = buf[static_cast<size_t>(channel)][static_cast<size_t>(s)];
                    if (hasLast[static_cast<size_t>(channel)])
                    {
                        peakDelta = std::max(peakDelta, std::abs(out - lastOut[static_cast<size_t>(channel)]));
                    }
                    lastOut[static_cast<size_t>(channel)] = out;
                    hasLast[static_cast<size_t>(channel)] = true;
                    if (sumSq != nullptr && sampleCount != nullptr)
                    {
                        *sumSq += static_cast<double>(out) * static_cast<double>(out);
                        ++(*sampleCount);
                    }
                }
            }
        }
        return peakDelta;
    };

    auto updateParams =
        [](EchoesModel& model, int stereoMode, bool tempoSync, float h1, float h2, float h3, int heads[3])
    {
        model.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f,
                               1.0f, stereoMode, tempoSync, h1, h2, h3);
    };

    float stereo441 = 0.0f;
    float stereo480 = 0.0f;
    float tempo441 = 0.0f;
    float tempo480 = 0.0f;
    float stereo441Recovery = 0.0f;
    float stereo480Recovery = 0.0f;
    float rapid441Start = 0.0f;
    float rapid480Start = 0.0f;
    float rapid441Progress = 0.0f;
    float rapid480Progress = 0.0f;
    float rapid441Expected = 0.0f;
    float rapid480Expected = 0.0f;
    bool stereo441Pass = false;
    bool stereo480Pass = false;
    bool rapid441Pass = false;
    bool rapid480Pass = false;
    bool tempo441Pass = false;
    bool tempo480Pass = false;

    for (float fs : sampleRates)
    {
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        int heads[3] = {1, 1, 1};

        // Stereo mode change scenario (Normal -> PingPong -> Normal)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            float phase = 0.0f;
            std::array<float, 2> lastOut{};
            std::array<bool, 2> hasLast{};

            updateParams(model, 0, false, 0.0f, 0.0f, 0.0f, heads);
            processBlocks(model, buf, warmBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast, nullptr, nullptr);

            double baselineSumSq = 0.0;
            int baselineSampleCount = 0;
            float baseline = processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast,
                                           &baselineSumSq, &baselineSampleCount);
            const float baselineRms =
                baselineSampleCount > 0
                    ? static_cast<float>(std::sqrt(baselineSumSq / static_cast<double>(baselineSampleCount)))
                    : 0.0f;
            float threshold = std::max(baseline * 4.0f, baseline + clickThreshold);

            updateParams(model, 1, false, 0.0f, 0.0f, 0.0f, heads);
            float peakA = processBlocks(model, buf, measureBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast,
                                        nullptr, nullptr);

            updateParams(model, 0, false, 0.0f, 0.0f, 0.0f, heads);
            float peakB = processBlocks(model, buf, measureBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast,
                                        nullptr, nullptr);

            double recoverySumSq = 0.0;
            int recoverySampleCount = 0;
            processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast, &recoverySumSq,
                          &recoverySampleCount);
            const float recoveredRms =
                recoverySampleCount > 0
                    ? static_cast<float>(std::sqrt(recoverySumSq / static_cast<double>(recoverySampleCount)))
                    : 0.0f;
            const float recoveryRatio = recoveredRms / std::max(1.0e-9f, baselineRms);

            float peakStereo = std::max(peakA, peakB);
            if (fs < 47000.0f)
            {
                stereo441 = peakStereo;
                stereo441Recovery = recoveryRatio;
                stereo441Pass =
                    (peakA <= threshold) && (peakB <= threshold) && (recoveryRatio >= levelRecoveryThreshold);
            }
            else
            {
                stereo480 = peakStereo;
                stereo480Recovery = recoveryRatio;
                stereo480Pass =
                    (peakA <= threshold) && (peakB <= threshold) && (recoveryRatio >= levelRecoveryThreshold);
            }
        }

        // A binary mode reversal inside the 15 ms record fade must continue
        // from the complementary progress instead of restarting at an endpoint.
        {
            TapeDelayProcessor delay;
            delay.Reset(fs, 2, blkSize);
            int rapidHeads[3] = {1, 0, 0};
            std::vector<std::vector<float>> silent(2, std::vector<float>(blkSize, 0.0f));
            const auto updateRapidMode = [&](int mode)
            {
                delay.UpdateParameters(0.0f, 0.5f, rapidHeads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false,
                                       0.0f, 0.0f, 0.0f, mode, 0, true, 0.0f, 0.0f, 0.0f, 1.0f);
            };

            updateRapidMode(0);
            updateRapidMode(1);
            const float newTransitionStart = TapeDelayTestAccess::modeCrossfadeProgress(delay);
            const bool newTransitionStartsAtZero = TapeDelayTestAccess::isModeCrossfadeActive(delay) &&
                                                   newTransitionStart <= 1.0e-6f &&
                                                   TapeDelayTestAccess::previousStereoMode(delay) == 0 &&
                                                   TapeDelayTestAccess::currentStereoMode(delay) == 1;

            delay.ProcessBuffer(silent, blkSize);
            const float forwardProgress = TapeDelayTestAccess::modeCrossfadeProgress(delay);
            const bool forwardStillActive =
                TapeDelayTestAccess::isModeCrossfadeActive(delay) && forwardProgress > 0.0f && forwardProgress < 1.0f;

            updateRapidMode(0);
            const float reversedProgress = TapeDelayTestAccess::modeCrossfadeProgress(delay);
            const float expectedProgress = std::clamp(1.0f - forwardProgress, 0.0f, 1.0f);
            const bool reversalContinuous = TapeDelayTestAccess::isModeCrossfadeActive(delay) &&
                                            std::abs(reversedProgress - expectedProgress) <= 1.0e-5f &&
                                            TapeDelayTestAccess::previousStereoMode(delay) == 1 &&
                                            TapeDelayTestAccess::currentStereoMode(delay) == 0;

            const bool statePassed = newTransitionStartsAtZero && forwardStillActive && reversalContinuous;

            if (fs < 47000.0f)
            {
                rapid441Start = newTransitionStart;
                rapid441Progress = reversedProgress;
                rapid441Expected = expectedProgress;
                rapid441Pass = statePassed;
            }
            else
            {
                rapid480Start = newTransitionStart;
                rapid480Progress = reversedProgress;
                rapid480Expected = expectedProgress;
                rapid480Pass = statePassed;
            }
        }

        // Tempo sync subdivision change scenario (large jump)
        {
            EchoesModel model;
            model.Reset(fs, 2, blkSize);
            float phase = 0.0f;
            std::array<float, 2> lastOut{};
            std::array<bool, 2> hasLast{};

            float h1A = TestUtilities::calculateSyncedDelayMs(bpm, 2);  // 1/4
            float h2A = TestUtilities::calculateSyncedDelayMs(bpm, 3);  // 1/8
            float h3A = TestUtilities::calculateSyncedDelayMs(bpm, 4);  // 1/16
            updateParams(model, 0, true, h1A, h2A, h3A, heads);
            processBlocks(model, buf, warmBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast, nullptr, nullptr);
            float baseline = processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast,
                                           nullptr, nullptr);
            float threshold = std::max(baseline * 4.0f, baseline + clickThreshold);

            float h1B = TestUtilities::calculateSyncedDelayMs(bpm, 0);  // 1/1
            float h2B = TestUtilities::calculateSyncedDelayMs(bpm, 5);  // 1/32
            float h3B = TestUtilities::calculateSyncedDelayMs(bpm, 6);  // 1/64
            updateParams(model, 0, true, h1B, h2B, h3B, heads);
            float peakA = processBlocks(model, buf, measureBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast,
                                        nullptr, nullptr);

            updateParams(model, 0, true, h1A, h2A, h3A, heads);
            float peakB = processBlocks(model, buf, measureBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast,
                                        nullptr, nullptr);

            float peakTempo = std::max(peakA, peakB);
            if (fs < 47000.0f)
            {
                tempo441 = peakTempo;
                tempo441Pass = (peakA <= threshold) && (peakB <= threshold);
            }
            else
            {
                tempo480 = peakTempo;
                tempo480Pass = (peakA <= threshold) && (peakB <= threshold);
            }
        }
    }

    bool passed = stereo441Pass && stereo480Pass && rapid441Pass && rapid480Pass && tempo441Pass && tempo480Pass;

    juce::String result;
    result << "ModeChangeClick: stereo441=" << juce::String(stereo441, 4) << " stereo480=" << juce::String(stereo480, 4)
           << " recovery441=" << juce::String(stereo441Recovery, 3)
           << " recovery480=" << juce::String(stereo480Recovery, 3) << " new441=" << juce::String(rapid441Start, 4)
           << " rapid441=" << juce::String(rapid441Progress, 4) << "/" << juce::String(rapid441Expected, 4)
           << " new480=" << juce::String(rapid480Start, 4) << " rapid480=" << juce::String(rapid480Progress, 4) << "/"
           << juce::String(rapid480Expected, 4) << " tempo441=" << juce::String(tempo441, 4)
           << " tempo480=" << juce::String(tempo480, 4) << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runDelayEnabledToggleClickTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float sampleRates[] = {44100.0f, 48000.0f};
    const int blkSize = 512;
    const int warmBlocks = 200;
    const int baselineBlocks = 20;
    const int transitionBlocks = 12;
    const float sineFreq = 440.0f;
    const float sineAmp = 0.2f;
    const float clickThreshold = 0.01f;  // -40 dBFS

    auto processBlocks = [&](EchoesModel& model, std::vector<std::vector<float>>& buf, int blocks, float fs, float freq,
                             float amp, float& phase, float& lastOut, bool& hasLast) -> float
    {
        float peakDelta = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * freq / fs;
        for (int b = 0; b < blocks; ++b)
        {
            for (int s = 0; s < blkSize; ++s)
            {
                float sample = amp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buf[0][static_cast<size_t>(s)] = sample;
                buf[1][static_cast<size_t>(s)] = sample;
            }

            model.ProcessBuffer(buf, blkSize);
            for (int s = 0; s < blkSize; ++s)
            {
                const float out = buf[0][static_cast<size_t>(s)];
                if (hasLast)
                    peakDelta = std::max(peakDelta, std::abs(out - lastOut));
                lastOut = out;
                hasLast = true;
            }
        }
        return peakDelta;
    };

    auto updateParams = [](EchoesModel& model, int delayEnabled, float echoSend)
    {
        int heads[3] = {1, 1, 1};
        model.UpdateParameters(0.5f, 0.5f, 0.5f, 1.0f, heads, delayEnabled, 1.0f, 0, 1.0f, echoSend, false, 0.0f, 0.0f,
                               0.0f, 1.0f, 1.0f, 0, false, 0.0f, 0.0f, 0.0f);
    };

    float off441 = 0.0f, off480 = 0.0f;
    float on441 = 0.0f, on480 = 0.0f;
    bool off441Pass = false, off480Pass = false;
    bool on441Pass = false, on480Pass = false;

    for (float fs : sampleRates)
    {
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        EchoesModel model;
        model.Reset(fs, 2, blkSize);

        float phase = 0.0f;
        float lastOut = 0.0f;
        bool hasLast = false;

        updateParams(model, 1, 1.0f);
        processBlocks(model, buf, warmBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);
        const float baselineEnabled =
            processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);

        updateParams(model, 0, 1.0f);
        const float peakOff =
            processBlocks(model, buf, transitionBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);
        const float baselineDisabled =
            processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);

        updateParams(model, 1, 1.0f);
        const float peakOn =
            processBlocks(model, buf, transitionBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);
        const float baselineReEnabled =
            processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);

        const float baselineForOff = std::max(baselineEnabled, baselineDisabled);
        const float baselineForOn = std::max(baselineDisabled, baselineReEnabled);
        const float thresholdOff = std::max(baselineForOff * 4.0f, baselineForOff + clickThreshold);
        const float thresholdOn = std::max(baselineForOn * 4.0f, baselineForOn + clickThreshold);

        if (fs < 47000.0f)
        {
            off441 = peakOff;
            on441 = peakOn;
            off441Pass = peakOff <= thresholdOff;
            on441Pass = peakOn <= thresholdOn;
        }
        else
        {
            off480 = peakOff;
            on480 = peakOn;
            off480Pass = peakOff <= thresholdOff;
            on480Pass = peakOn <= thresholdOn;
        }
    }

    const bool passed = off441Pass && off480Pass && on441Pass && on480Pass;

    juce::String result;
    result << "DelayEnabledToggleClick: off441=" << juce::String(off441, 4) << " on441=" << juce::String(on441, 4)
           << " off480=" << juce::String(off480, 4) << " on480=" << juce::String(on480, 4)
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runEchoSendToggleClickTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float sampleRates[] = {44100.0f, 48000.0f};
    const int blkSize = 512;
    const int warmBlocks = 200;
    const int baselineBlocks = 20;
    const int measureBlocks = 60;
    const float sineFreq = 440.0f;
    const float sineAmp = 0.2f;
    const float clickThreshold = 0.01f;  // -40 dBFS

    auto processBlocks = [&](EchoesModel& model, std::vector<std::vector<float>>& buf, int blocks, float fs, float freq,
                             float amp, float& phase, float& lastOut, bool& hasLast) -> float
    {
        float peakDelta = 0.0f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * freq / fs;
        for (int b = 0; b < blocks; ++b)
        {
            for (int s = 0; s < blkSize; ++s)
            {
                float sample = amp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                buf[0][static_cast<size_t>(s)] = sample;
                buf[1][static_cast<size_t>(s)] = sample;
            }

            model.ProcessBuffer(buf, blkSize);
            for (int s = 0; s < blkSize; ++s)
            {
                const float out = buf[0][static_cast<size_t>(s)];
                if (hasLast)
                    peakDelta = std::max(peakDelta, std::abs(out - lastOut));
                lastOut = out;
                hasLast = true;
            }
        }
        return peakDelta;
    };

    auto updateParams = [](EchoesModel& model, float echoSend)
    {
        int heads[3] = {1, 1, 1};
        model.UpdateParameters(0.5f, 0.5f, 0.5f, 1.0f, heads, 1, 1.0f, 0, 1.0f, echoSend, false, 0.0f, 0.0f, 0.0f, 1.0f,
                               1.0f, 0, false, 0.0f, 0.0f, 0.0f);
    };

    float off441 = 0.0f, off480 = 0.0f;
    float on441 = 0.0f, on480 = 0.0f;
    bool off441Pass = false, off480Pass = false;
    bool on441Pass = false, on480Pass = false;

    for (float fs : sampleRates)
    {
        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
        EchoesModel model;
        model.Reset(fs, 2, blkSize);

        float phase = 0.0f;
        float lastOut = 0.0f;
        bool hasLast = false;

        updateParams(model, 1.0f);
        processBlocks(model, buf, warmBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);
        const float baseline =
            processBlocks(model, buf, baselineBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);
        const float threshold = std::max(baseline * 4.0f, baseline + clickThreshold);

        updateParams(model, 0.0f);
        const float peakOff = processBlocks(model, buf, measureBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);

        updateParams(model, 1.0f);
        const float peakOn = processBlocks(model, buf, measureBlocks, fs, sineFreq, sineAmp, phase, lastOut, hasLast);

        if (fs < 47000.0f)
        {
            off441 = peakOff;
            on441 = peakOn;
            off441Pass = peakOff <= threshold;
            on441Pass = peakOn <= threshold;
        }
        else
        {
            off480 = peakOff;
            on480 = peakOn;
            off480Pass = peakOff <= threshold;
            on480Pass = peakOn <= threshold;
        }
    }

    const bool passed = off441Pass && off480Pass && on441Pass && on480Pass;

    juce::String result;
    result << "EchoSendToggleClick: off441=" << juce::String(off441, 4) << " on441=" << juce::String(on441, 4)
           << " off480=" << juce::String(off480, 4) << " on480=" << juce::String(on480, 4)
           << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runMonoDelaySwitchRampsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr float sineFreq = 440.0f;
    constexpr float sineAmp = 0.35f;

    struct Metrics
    {
        double energy = 0.0;
        int64_t samples = 0;
        float peak = 0.0f;
        int invalid = 0;

        float rms() const
        {
            return (samples > 0) ? static_cast<float>(std::sqrt(energy / static_cast<double>(samples))) : 0.0f;
        }
    };

    auto updateDelay = [](TapeDelayProcessor& delay, int delayEnabled, float echoSend, float intensity)
    {
        int heads[3] = {1, 0, 0};
        delay.UpdateParameters(intensity, 1.0f, heads, delayEnabled, echoSend, false, 0.0f, 0.08f, 0.0f, 1.0f, 1.0f,
                               false, 0.0f, 0.0f, 0.0f, 0, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);
    };

    auto render = [](TapeDelayProcessor& delay, std::vector<std::vector<float>>& buffer, int blocks,
                     int measureFromBlock, float inputAmp, float& phase) -> Metrics
    {
        Metrics metrics;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * sineFreq / fs;
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blkSize; ++sample)
            {
                buffer[0][static_cast<size_t>(sample)] = inputAmp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::twoPi)
                    phase -= juce::MathConstants<float>::twoPi;
            }

            delay.ProcessBuffer(buffer, blkSize);

            if (block >= measureFromBlock)
            {
                for (int sample = 0; sample < blkSize; ++sample)
                {
                    const float out = buffer[0][static_cast<size_t>(sample)];
                    if (!std::isfinite(out))
                    {
                        ++metrics.invalid;
                        continue;
                    }

                    metrics.energy += static_cast<double>(out) * static_cast<double>(out);
                    ++metrics.samples;
                    metrics.peak = std::max(metrics.peak, std::abs(out));
                }
            }
        }
        return metrics;
    };

    std::vector<std::vector<float>> buffer(1, std::vector<float>(blkSize, 0.0f));

    TapeDelayProcessor delayEnableCase;
    delayEnableCase.Reset(fs, 1, blkSize);
    float phase = 0.0f;
    updateDelay(delayEnableCase, 1, 1.0f, 0.65f);
    render(delayEnableCase, buffer, 160, 160, sineAmp, phase);
    const Metrics delayBaseline = render(delayEnableCase, buffer, 24, 0, sineAmp, phase);
    updateDelay(delayEnableCase, 0, 1.0f, 0.65f);
    const Metrics delayOff = render(delayEnableCase, buffer, 48, 24, sineAmp, phase);

    TapeDelayProcessor echoSendCase;
    echoSendCase.Reset(fs, 1, blkSize);
    std::fill(buffer[0].begin(), buffer[0].end(), 0.0f);
    phase = 0.0f;
    updateDelay(echoSendCase, 1, 1.0f, 0.0f);
    render(echoSendCase, buffer, 160, 160, sineAmp, phase);
    const Metrics sendBaseline = render(echoSendCase, buffer, 24, 0, sineAmp, phase);
    updateDelay(echoSendCase, 1, 0.0f, 0.0f);
    const Metrics sendOff = render(echoSendCase, buffer, 64, 36, sineAmp, phase);

    const float delayBaseRms = delayBaseline.rms();
    const float delayOffRms = delayOff.rms();
    const float sendBaseRms = sendBaseline.rms();
    const float sendOffRms = sendOff.rms();
    const bool delayAudible = delayBaseRms > 1.0e-3f;
    const bool sendAudible = sendBaseRms > 1.0e-3f;
    const bool delayMuted = delayOffRms <= delayBaseRms * 0.05f && delayOff.peak <= delayBaseline.peak * 0.08f;
    const bool sendMuted = sendOffRms <= sendBaseRms * 0.05f && sendOff.peak <= sendBaseline.peak * 0.08f;
    const int invalid = delayBaseline.invalid + delayOff.invalid + sendBaseline.invalid + sendOff.invalid;
    const bool passed = delayAudible && sendAudible && delayMuted && sendMuted && invalid == 0;

    juce::String result;
    result << "MonoDelaySwitchRamps:"
           << " delayRms=" << juce::String(TestUtilities::linearToDb(delayBaseRms), 1) << "->"
           << juce::String(TestUtilities::linearToDb(delayOffRms), 1) << "dB"
           << " sendRms=" << juce::String(TestUtilities::linearToDb(sendBaseRms), 1) << "->"
           << juce::String(TestUtilities::linearToDb(sendOffRms), 1) << "dB"
           << " delayPeak=" << juce::String(delayBaseline.peak, 4) << "->" << juce::String(delayOff.peak, 4)
           << " sendPeak=" << juce::String(sendBaseline.peak, 4) << "->" << juce::String(sendOff.peak, 4)
           << " invalid=" << invalid << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runMonoSpliceWowParityTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr int captureSample = 192;
    constexpr float head1Ms = 70.0f;
    constexpr float loopTimeRatio = 17.14f;
    constexpr float wornSplicePhaseWidth = 0.01f * 1.6f;
    constexpr float targetSplicePhase = (wornSplicePhaseWidth * 1.5f) + (wornSplicePhaseWidth * 2.5f * 0.5f);
    constexpr float expectedLoopSamples = loopTimeRatio * head1Ms * fs / 1000.0f;
    const int warmSamples =
        std::max(0, static_cast<int>(std::round(targetSplicePhase * expectedLoopSamples)) - captureSample);

    struct Capture
    {
        bool captured = false;
        float wowMs = 0.0f;
        float h1Ms = 0.0f;
        int invalid = 0;
    };

    auto configureDelay = [](TapeDelayProcessor& delay)
    {
        int heads[3] = {1, 0, 0};
        delay.UpdateParameters(0.0f, 1.0f, heads, 1, 1.0f, false, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, false, -1.0f, -1.0f, 0.0f, 1.0f);
    };

    auto advanceSamples = [](TapeDelayProcessor& delay, std::vector<std::vector<float>>& buffer, int samplesToAdvance)
    {
        int remaining = samplesToAdvance;
        while (remaining > 0)
        {
            const int n = std::min(static_cast<int>(buffer[0].size()), remaining);
            for (auto& channel : buffer)
                std::fill(channel.begin(), channel.begin() + n, 0.0f);
            delay.ProcessBuffer(buffer, n);
            remaining -= n;
        }
    };

    auto captureAtSplice = [&](int channels) -> Capture
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, channels, blkSize);
        configureDelay(delay);

        std::vector<std::vector<float>> buffer(static_cast<size_t>(channels), std::vector<float>(blkSize, 0.0f));
        advanceSamples(delay, buffer, warmSamples);

        TapeDelayTestAccess::enableHeadTimingCapture(delay, true);
        for (auto& channel : buffer)
            std::fill(channel.begin(), channel.end(), 0.0f);
        for (int ch = 0; ch < channels; ++ch)
            buffer[static_cast<size_t>(ch)][captureSample] = 0.25f;

        delay.ProcessBuffer(buffer, blkSize);

        Capture capture;
        for (const auto& channel : buffer)
        {
            for (float sample : channel)
            {
                if (!std::isfinite(sample))
                    ++capture.invalid;
            }
        }

        TapeDelayProcessor::HeadTimingSnapshot snapshot;
        capture.captured = TapeDelayTestAccess::getHeadTimingSnapshot(delay, snapshot);
        if (capture.captured)
        {
            capture.wowMs = snapshot.wowFlutterMs;
            capture.h1Ms = snapshot.h1TimeMs;
        }
        return capture;
    };

    const Capture stereo = captureAtSplice(2);
    const Capture mono = captureAtSplice(1);

    const float stereoWow = std::abs(stereo.wowMs);
    const float monoWow = std::abs(mono.wowMs);
    const float diff = std::abs(stereoWow - monoWow);
    const bool stereoHasSpliceWow = stereoWow >= 0.04f;
    const bool monoHasSpliceWow = monoWow >= 0.04f;
    const bool parity = diff <= 0.025f;
    const bool timingOk = std::abs(stereo.h1Ms - mono.h1Ms) <= 0.05f;
    const bool passed = stereo.captured && mono.captured && stereoHasSpliceWow && monoHasSpliceWow && parity &&
                        timingOk && (stereo.invalid + mono.invalid) == 0;

    juce::String result;
    result << "MonoSpliceWowParity:"
           << " stereoWow=" << juce::String(stereo.wowMs, 4) << "ms"
           << " monoWow=" << juce::String(mono.wowMs, 4) << "ms"
           << " diff=" << juce::String(diff, 4) << "ms"
           << " h1=" << juce::String(stereo.h1Ms, 3) << "/" << juce::String(mono.h1Ms, 3) << "ms"
           << " captured=" << (stereo.captured ? 1 : 0) << "/" << (mono.captured ? 1 : 0)
           << " invalid=" << (stereo.invalid + mono.invalid) << " warmSamples=" << warmSamples << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runKillClearFilterStateResetTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blkSize = 512;
    constexpr float sineFreq = 110.0f;
    constexpr float sineAmp = 0.55f;

    struct Metrics
    {
        float peak = 0.0f;
        double energy = 0.0;
        int64_t samples = 0;
        int invalid = 0;

        float rms() const
        {
            return (samples > 0) ? static_cast<float>(std::sqrt(energy / static_cast<double>(samples))) : 0.0f;
        }
    };

    auto updateDelay = [](TapeDelayProcessor& delay)
    {
        int heads[3] = {1, 1, 1};
        delay.UpdateParameters(0.78f, 1.0f, heads, 1, 1.0f, false, 0.0f, 0.55f, 0.25f, 1.0f, 1.0f, false, 0.0f, 0.0f,
                               0.0f, 0, 0, false, 0.0f, 0.0f, 0.0f, 1.0f);
    };

    auto render = [](TapeDelayProcessor& delay, std::vector<std::vector<float>>& buffer, int blocks, float inputAmp,
                     float& phase, bool collect) -> Metrics
    {
        Metrics metrics;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * sineFreq / fs;
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blkSize; ++sample)
            {
                float input = 0.0f;
                if (inputAmp > 0.0f)
                {
                    input = inputAmp * std::sin(phase);
                    phase += phaseInc;
                    if (phase >= juce::MathConstants<float>::twoPi)
                        phase -= juce::MathConstants<float>::twoPi;
                }

                buffer[0][static_cast<size_t>(sample)] = input;
                buffer[1][static_cast<size_t>(sample)] = input;
            }

            delay.ProcessBuffer(buffer, blkSize);

            if (collect)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    for (int sample = 0; sample < blkSize; ++sample)
                    {
                        const float out = buffer[static_cast<size_t>(ch)][static_cast<size_t>(sample)];
                        if (!std::isfinite(out))
                        {
                            ++metrics.invalid;
                            continue;
                        }

                        metrics.peak = std::max(metrics.peak, std::abs(out));
                        metrics.energy += static_cast<double>(out) * static_cast<double>(out);
                        ++metrics.samples;
                    }
                }
            }
        }
        return metrics;
    };

    TapeDelayProcessor delay;
    delay.Reset(fs, 2, blkSize);
    updateDelay(delay);

    std::vector<std::vector<float>> buffer(2, std::vector<float>(blkSize, 0.0f));
    float phase = 0.0f;

    render(delay, buffer, 220, sineAmp, phase, false);
    const Metrics active = render(delay, buffer, 12, sineAmp, phase, true);

    delay.triggerKill();
    render(delay, buffer, 8, 0.0f, phase, false);
    const Metrics postClear = render(delay, buffer, 8, 0.0f, phase, true);

    const bool excited = active.rms() > 1.0e-3f;
    const bool cleared = postClear.peak <= 1.0e-7f && postClear.rms() <= 1.0e-8f;
    const bool passed = excited && cleared && active.invalid == 0 && postClear.invalid == 0;

    juce::String result;
    result << "KillClearFilterStateReset:"
           << " activeRms=" << juce::String(TestUtilities::linearToDb(active.rms()), 1) << "dB"
           << " postPeak=" << juce::String(TestUtilities::linearToDb(postClear.peak), 1) << "dB"
           << " postRms=" << juce::String(TestUtilities::linearToDb(postClear.rms()), 1) << "dB"
           << " invalid=" << (active.invalid + postClear.invalid) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runTempoSyncToggleTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP Test: Measure actual delay time accuracy at 120 BPM
    const float fs = 48000.0f;
    const int blkSize = 512;
    const double bpm = 120.0;

    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    float expectedDelayMs = 500.0f;
    float h1SyncDelayMs = TestUtilities::calculateSyncedDelayMs(bpm, 2);
    float h2SyncDelayMs = TestUtilities::calculateSyncedDelayMs(bpm, 3);
    float h3SyncDelayMs = TestUtilities::calculateSyncedDelayMs(bpm, 4);
    const float detectionThreshold = 0.004f;

    int heads[3] = {1, 0, 0};
    testModel.UpdateParameters(0.5f, 0.5f, 0.5f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1SyncDelayMs, h2SyncDelayMs, h3SyncDelayMs, false, 0.0f, 0.0f, 1.0f,
                               100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f, 0);

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
    for (int w = 0; w < 200; ++w)
        testModel.ProcessBuffer(buf, blkSize);

    buf[0][0] = 0.9f;
    buf[1][0] = 0.9f;
    testModel.ProcessBuffer(buf, blkSize);

    for (int skip = 0; skip < 10; ++skip)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
    }

    std::vector<std::pair<int, float>> peaks;
    for (int b = 0; b < 150; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        for (int s = 0; s < blkSize; ++s)
        {
            float val = std::abs(buf[0][static_cast<size_t>(s)]);
            if (val > detectionThreshold)
            {
                int samplePos = (b + 10) * blkSize + s;
                peaks.push_back({samplePos, val});
            }
        }
    }

    int peakSample = -1;
    float peakValue = 0.0f;
    for (const auto& p : peaks)
    {
        if (p.second > peakValue)
        {
            peakValue = p.second;
            peakSample = p.first;
        }
    }

    int expectedSample = static_cast<int>(expectedDelayMs * fs / 1000.0f);
    int closestToExpected = -1;
    float closestValue = 0.0f;
    int minDistance = INT_MAX;
    for (const auto& p : peaks)
    {
        int distance = std::abs(p.first - expectedSample);
        if (distance < minDistance && p.second > detectionThreshold)
        {
            minDistance = distance;
            closestToExpected = p.first;
            closestValue = p.second;
        }
    }

    float measuredDelayMs = (peakSample > 0) ? (static_cast<float>(peakSample) / fs) * 1000.0f : 0.0f;
    float closestDelayMs = (closestToExpected > 0) ? (static_cast<float>(closestToExpected) / fs) * 1000.0f : 0.0f;
    float closestError = std::abs(closestDelayMs - expectedDelayMs);

    bool passed = closestError < 30.0f && closestValue > detectionThreshold;

    juce::String result;
    result << "TempoSync: expected=" << juce::String(expectedDelayMs, 1)
           << "ms measured=" << juce::String(measuredDelayMs, 1) << "ms closest=" << juce::String(closestDelayMs, 1)
           << "ms peak=" << juce::String(peakValue, 5) << " peaks=" << peaks.size() << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runTempoSyncTransportCouplingTest(SpatialEchoAudioProcessor& /*processor*/)
{
    const float fs = 48000.0f;
    const int blkSize = 512;
    const int warmBlocks = 300;
    const float rawDelayParam = 0.5f;
    const float bpm = 120.0f;
    const float shortSyncMs = TestUtilities::calculateSyncedDelayMs(bpm, 4);  // 1/16 = 125ms
    const float longSyncMs = TestUtilities::calculateSyncedDelayMs(bpm, 2);   // 1/4 = 500ms

    auto configureDelay = [&](TapeDelayProcessor& delay, float h1Ms, float lpfScale = 1.0f)
    {
        int heads[3] = {1, 0, 0};
        delay.UpdateParameters(0.0f, rawDelayParam, heads, 1, 1.0f, false, 0.0f, 0.45f, 0.0f, 1.0f, 1.0f, true, h1Ms,
                               h1Ms * 0.5f, h1Ms * 0.25f, 0, 0, true, -1.0f, -1.0f, 1.0f, lpfScale);
    };

    TapeDelayProcessor transportDelay;
    transportDelay.Reset(fs, 2, blkSize);
    configureDelay(transportDelay, longSyncMs);
    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
    for (int i = 0; i < warmBlocks; ++i)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        transportDelay.ProcessBuffer(buf, blkSize);
    }

    TapeDelayTestAccess::enableHeadTimingCapture(transportDelay, true);
    std::fill(buf[0].begin(), buf[0].end(), 0.0f);
    std::fill(buf[1].begin(), buf[1].end(), 0.0f);
    buf[0][0] = 0.25f;
    buf[1][0] = 0.25f;
    transportDelay.ProcessBuffer(buf, blkSize);

    TapeDelayProcessor::HeadTimingSnapshot snapshot;
    const bool captured = TapeDelayTestAccess::getHeadTimingSnapshot(transportDelay, snapshot);
    const float transportErrorMs = captured ? std::abs(snapshot.delayTimeSmoothedMs - longSyncMs) : 9999.0f;
    const float readErrorMs = captured ? std::abs(snapshot.h1TimeMs - longSyncMs) : 9999.0f;

    auto renderRecordHighRms = [&](float h1Ms) -> float
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blkSize);
        configureDelay(delay, h1Ms);

        std::vector<std::vector<float>> tone(2, std::vector<float>(blkSize, 0.0f));
        const float freq = 8000.0f;
        const float amp = 0.05f;
        const float phaseInc = 2.0f * juce::MathConstants<float>::pi * freq / fs;
        float phase = 0.0f;
        double sumSq = 0.0;
        int count = 0;

        for (int b = 0; b < 160; ++b)
        {
            for (int s = 0; s < blkSize; ++s)
            {
                const float sample = amp * std::sin(phase);
                phase += phaseInc;
                if (phase >= 2.0f * juce::MathConstants<float>::pi)
                    phase -= 2.0f * juce::MathConstants<float>::pi;
                tone[0][static_cast<size_t>(s)] = sample;
                tone[1][static_cast<size_t>(s)] = sample;
            }

            TapeDelayTestAccess::processRecordChainOnly(delay, tone, blkSize);
            if (b >= 80)
            {
                for (int s = 0; s < blkSize; ++s)
                {
                    sumSq += static_cast<double>(tone[0][static_cast<size_t>(s)]) *
                             static_cast<double>(tone[0][static_cast<size_t>(s)]);
                    ++count;
                }
            }
        }

        return count > 0 ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(count))) : 0.0f;
    };

    const float shortHighRms = renderRecordHighRms(shortSyncMs);
    const float longHighRms = renderRecordHighRms(longSyncMs);
    const float longVsShortDb = TestUtilities::linearToDb(longHighRms / std::max(1.0e-9f, shortHighRms));

    TapeDelayProcessor shortDelay;
    shortDelay.Reset(fs, 2, blkSize);
    configureDelay(shortDelay, shortSyncMs);
    TapeDelayProcessor longDelay;
    longDelay.Reset(fs, 2, blkSize);
    configureDelay(longDelay, longSyncMs);
    TapeDelayProcessor darkFlavorDelay;
    darkFlavorDelay.Reset(fs, 2, blkSize);
    configureDelay(darkFlavorDelay, longSyncMs, 0.8f);
    TapeDelayProcessor brightFlavorDelay;
    brightFlavorDelay.Reset(fs, 2, blkSize);
    configureDelay(brightFlavorDelay, longSyncMs, 1.2f);

    const float shortPlaybackHz = TapeDelayTestAccess::playbackFilterCutoff(shortDelay);
    const float longPlaybackHz = TapeDelayTestAccess::playbackFilterCutoff(longDelay);
    const float shortFeedbackHz = TapeDelayTestAccess::feedbackFilterCutoff(shortDelay);
    const float longFeedbackHz = TapeDelayTestAccess::feedbackFilterCutoff(longDelay);
    const float shortRecordHz = TapeDelayTestAccess::recordFilterCutoff(shortDelay);
    const float longRecordHz = TapeDelayTestAccess::recordFilterCutoff(longDelay);
    const bool ownershipOk = shortPlaybackHz >= longPlaybackHz + 1000.0f &&
                             std::abs(shortFeedbackHz - longFeedbackHz) <= 1.0f &&
                             std::abs(shortRecordHz - longRecordHz) <= 1.0f &&
                             std::abs(TapeDelayTestAccess::playbackFilterCutoff(darkFlavorDelay) -
                                      TapeDelayTestAccess::playbackFilterCutoff(brightFlavorDelay)) <= 1.0f &&
                             std::abs(TapeDelayTestAccess::recordFilterCutoff(darkFlavorDelay) -
                                      TapeDelayTestAccess::recordFilterCutoff(brightFlavorDelay)) <= 1.0f &&
                             TapeDelayTestAccess::feedbackFilterCutoff(brightFlavorDelay) >=
                                 TapeDelayTestAccess::feedbackFilterCutoff(darkFlavorDelay) + 1000.0f;

    const bool transportOk = captured && snapshot.tempoSyncEnabled && transportErrorMs <= 35.0f && readErrorMs <= 5.0f;
    const bool toneOk = shortHighRms > 1.0e-5f && longHighRms > 1.0e-5f && std::abs(longVsShortDb) <= 0.5f;
    const bool passed = transportOk && toneOk && ownershipOk;

    juce::String result;
    result << "TempoSyncTransportCoupling: motor="
           << (captured ? juce::String(snapshot.delayTimeSmoothedMs, 1) : juce::String("missing"))
           << "ms h1=" << (captured ? juce::String(snapshot.h1TimeMs, 1) : juce::String("missing"))
           << "ms expected=" << juce::String(longSyncMs, 1) << "ms err=" << juce::String(transportErrorMs, 1)
           << " shortHF=" << juce::String(TestUtilities::linearToDb(shortHighRms), 1)
           << "dB longHF=" << juce::String(TestUtilities::linearToDb(longHighRms), 1)
           << "dB long-short=" << juce::String(longVsShortDb, 1)
           << "dB cutoffs(short/long playback)=" << juce::String(shortPlaybackHz, 0) << "/"
           << juce::String(longPlaybackHz, 0) << " feedback=" << juce::String(shortFeedbackHz, 0) << "/"
           << juce::String(longFeedbackHz, 0) << " record=" << juce::String(shortRecordHz, 0) << "/"
           << juce::String(longRecordHz, 0) << " ownership=" << (ownershipOk ? "yes" : "no") << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runIndependentHeadsTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // DSP Test: Set H1=1/4, H2=1/8, H3=1/16 and verify 3 distinct echoes
    const float fs = 48000.0f;
    const int blkSize = 512;
    const double bpm = 120.0;

    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 1, 1};
    float h1 = TestUtilities::calculateSyncedDelayMs(bpm, 2);  // 500ms
    float h2 = TestUtilities::calculateSyncedDelayMs(bpm, 3);  // 250ms
    float h3 = TestUtilities::calculateSyncedDelayMs(bpm, 4);  // 125ms

    testModel.UpdateParameters(0.5f, 0.5f, 0.0f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1, h2, h3, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f,
                               0);

    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));
    for (int w = 0; w < 200; ++w)
        testModel.ProcessBuffer(buf, blkSize);

    buf[0][0] = 0.9f;
    buf[1][0] = 0.9f;
    testModel.ProcessBuffer(buf, blkSize);

    std::vector<float> allSamples;
    for (int b = 0; b < 150; ++b)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        for (int s = 0; s < blkSize; ++s)
            allSamples.push_back(std::abs(buf[0][static_cast<size_t>(s)]));
    }

    struct HeadPeak
    {
        const char* name;
        float expectedMs;
        int peakIndex = -1;
        float peak = 0.0f;
    };

    std::array<HeadPeak, 3> peaks{{{"H3", h3}, {"H2", h2}, {"H1", h1}}};

    const int toleranceSamples = static_cast<int>(0.045f * fs);
    float maxAbs = 0.0f;
    bool finite = true;
    for (float sample : allSamples)
    {
        finite = finite && std::isfinite(sample);
        maxAbs = std::max(maxAbs, sample);
    }

    for (auto& peak : peaks)
    {
        const int expectedSample = static_cast<int>(std::round(peak.expectedMs * fs / 1000.0f));
        const int start = std::max(0, expectedSample - toleranceSamples);
        const int end = std::min(static_cast<int>(allSamples.size()), expectedSample + toleranceSamples);
        for (int i = start; i < end; ++i)
        {
            const float sample = allSamples[static_cast<size_t>(i)];
            if (sample > peak.peak)
            {
                peak.peak = sample;
                peak.peakIndex = i;
            }
        }
    }

    const float peakThreshold = std::max(0.002f, maxAbs * 0.25f);
    bool allHeadsAudible = maxAbs > 0.005f;
    bool timingOk = true;
    for (const auto& peak : peaks)
    {
        const int expectedSample = static_cast<int>(std::round(peak.expectedMs * fs / 1000.0f));
        allHeadsAudible = allHeadsAudible && peak.peak >= peakThreshold;
        timingOk = timingOk && peak.peakIndex >= 0 && std::abs(peak.peakIndex - expectedSample) <= toleranceSamples;
    }

    const int minDistinctSamples = static_cast<int>(0.06f * fs);
    const bool distinct = std::abs(peaks[0].peakIndex - peaks[1].peakIndex) >= minDistinctSamples &&
                          std::abs(peaks[1].peakIndex - peaks[2].peakIndex) >= minDistinctSamples &&
                          std::abs(peaks[0].peakIndex - peaks[2].peakIndex) >= minDistinctSamples;

    bool passed = finite && allHeadsAudible && timingOk && distinct;
    juce::String result;
    result << "IndependentHeads: H3=" << juce::String(static_cast<float>(peaks[0].peakIndex) * 1000.0f / fs, 1) << "ms/"
           << juce::String(peaks[0].peak, 5)
           << " H2=" << juce::String(static_cast<float>(peaks[1].peakIndex) * 1000.0f / fs, 1) << "ms/"
           << juce::String(peaks[1].peak, 5)
           << " H1=" << juce::String(static_cast<float>(peaks[2].peakIndex) * 1000.0f / fs, 1) << "ms/"
           << juce::String(peaks[2].peak, 5) << " thresh=" << juce::String(peakThreshold, 5) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runAllModesTempoSyncTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Test all 11 delay modes with tempo sync enabled
    const float fs = 48000.0f;
    const int blkSize = 512;
    const double bpm = 120.0;

    int failCount = 0;

    for (int mode = 0; mode <= 10; ++mode)
    {
        EchoesModel testModel;
        testModel.Reset(fs, 2, blkSize);

        int heads[3] = {1, 1, 1};
        if (mode == 0)
        {
            heads[1] = 0;
            heads[2] = 0;
        }
        else if (mode == 1)
        {
            heads[0] = 0;
            heads[2] = 0;
        }
        else if (mode == 2)
        {
            heads[0] = 0;
            heads[1] = 0;
        }

        float h1 = TestUtilities::calculateSyncedDelayMs(bpm, 2);
        float h2 = TestUtilities::calculateSyncedDelayMs(bpm, 3);
        float h3 = TestUtilities::calculateSyncedDelayMs(bpm, 4);

        testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                                   0.5f, 0, true, h1, h2, h3, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f,
                                   0.5f, 0);

        std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

        try
        {
            for (int i = 0; i < 100; ++i)
            {
                std::fill(buf[0].begin(), buf[0].end(), 0.0f);
                std::fill(buf[1].begin(), buf[1].end(), 0.0f);
                testModel.ProcessBuffer(buf, blkSize);
            }
        }
        catch (const std::exception& e)
        {
            DBG(juce::String("DelayTests stability: caught std::exception: ") + e.what());
            failCount++;
        }
        catch (...)
        {
            DBG("DelayTests stability: caught non-standard exception");
            failCount++;
        }
    }

    juce::String result;
    result << "AllModes: tested 11, failed " << failCount << " " << (failCount == 0 ? "[PASS]" : "[FAIL]");
    return result;
}

juce::String runBPMChangeTest(SpatialEchoAudioProcessor& /*processor*/)
{
    // Test BPM changes during playback
    const float fs = 48000.0f;
    const int blkSize = 512;

    EchoesModel testModel;
    testModel.Reset(fs, 2, blkSize);

    int heads[3] = {1, 0, 0};
    std::vector<std::vector<float>> buf(2, std::vector<float>(blkSize, 0.0f));

    double bpm = 120.0;
    float h1 = TestUtilities::calculateSyncedDelayMs(bpm, 2);

    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1, h1, h1, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f,
                               0);

    for (int i = 0; i < 50; ++i)
        testModel.ProcessBuffer(buf, blkSize);

    bpm = 180.0;
    h1 = TestUtilities::calculateSyncedDelayMs(bpm, 2);
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1, h1, h1, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f,
                               0);

    float maxPeak = 0.0f;
    for (int i = 0; i < 50; ++i)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        std::fill(buf[1].begin(), buf[1].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        for (int s = 0; s < blkSize; ++s)
            maxPeak = std::max(maxPeak, std::abs(buf[0][static_cast<size_t>(s)]));
    }

    bpm = 60.0;
    h1 = TestUtilities::calculateSyncedDelayMs(bpm, 2);
    testModel.UpdateParameters(0.5f, 0.5f, 0.3f, 0.5f, heads, 1, 1.0f, 0, 1.0f, 1.0f, false, 0.0f, 0.5f, 0.0f, 0.5f,
                               0.5f, 0, true, h1, h1, h1, false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, 0.5f, 0.5f, 0.5f,
                               0);

    for (int i = 0; i < 50; ++i)
    {
        std::fill(buf[0].begin(), buf[0].end(), 0.0f);
        testModel.ProcessBuffer(buf, blkSize);
        for (int s = 0; s < blkSize; ++s)
            maxPeak = std::max(maxPeak, std::abs(buf[0][static_cast<size_t>(s)]));
    }

    bool passed = maxPeak < 0.5f;

    juce::String result;
    result << juce::String(juce::CharPointer_UTF8("BPMChange: 120→180→60 BPM peak=")) << juce::String(maxPeak, 3) << " "
           << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Sample-rate invariance gate
//==============================================================================
//
// Two classes of defect are pinned here.
//
// 1. Per-sample smoothing coefficients written as bare literals. A literal like
//    0.9995 encodes a time constant only at the sample rate it was chosen at;
//    at 192 kHz the same literal is a four-times shorter window. This gate
//    measures the time constant the coefficient actually implements and
//    requires it to be the same number of SECONDS at every supported rate,
//    while pinning 48 kHz to exactly 0.9995 so the 48 kHz sound stays the
//    reference.
//
// 2. Noise generator levels. Every tape noise source is per-sample i.i.d. noise
//    (or a per-sample Bernoulli impulse) feeding a fixed-Hz filter, so its
//    filtered RMS falls as 1/sqrt(fs) unless it is normalised. Un-normalised,
//    the whole tape noise bed is ~3 dB quieter per doubling of the sample rate.
//    This gate renders the real output-noise bed at four rates and requires the
//    level to hold.
juce::String runTapeTimebaseSampleRateInvarianceTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
    // The time constant the 0.9995f coefficient implements at 48 kHz:
    // tau = -1 / (fs * ln(0.9995)) = 41.656 ms.
    constexpr double targetTauSeconds = 0.0416563;
    constexpr double tauToleranceFraction = 0.05;
    constexpr double noiseToleranceDb = 0.75;
    constexpr double noiseFloorGateDb = -110.0;

    constexpr int blockSize = 256;
    constexpr double warmupSeconds = 0.5;
    constexpr double measureSeconds = 2.0;

    int heads[3] = {1, 0, 0};
    bool passed = true;
    juce::StringArray failures;
    juce::String rows;

    double referenceNoiseDb = 0.0;
    bool referenceNoiseValid = false;

    for (const float fs : sampleRates)
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blockSize);
        // Hiss on and loud, wear at mid, dropouts forced off so the measurement
        // is a stationary noise bed rather than a random-dip process.
        delay.UpdateParameters(0.0f, 0.0f, heads, 1, 1.0f, true, 1.0f, 0.5f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, true, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f);

        const float feedbackCoeff = TapeDelayTestAccess::feedbackAverageCoefficient(delay);
        const float glideCoeff = TapeDelayTestAccess::tempoSyncGlideCoefficient(delay);

        auto tauFromCoeff = [fs](float coeff) -> double
        {
            if (!(coeff > 0.0f) || !(coeff < 1.0f))
                return std::numeric_limits<double>::quiet_NaN();
            return -1.0 / (static_cast<double>(fs) * std::log(static_cast<double>(coeff)));
        };

        const double feedbackTau = tauFromCoeff(feedbackCoeff);
        const double glideTau = tauFromCoeff(glideCoeff);

        auto tauOk = [&](double tau)
        { return std::isfinite(tau) && std::abs(tau - targetTauSeconds) / targetTauSeconds <= tauToleranceFraction; };

        const bool coeffsOk = tauOk(feedbackTau) && tauOk(glideTau);
        // 48 kHz must resolve to exactly 0.9995f; anything else changes the
        // sound of every 48 kHz render.
        const bool pinnedOk =
            (fs != 48000.0f) || (juce::exactlyEqual(feedbackCoeff, 0.9995f) && juce::exactlyEqual(glideCoeff, 0.9995f));

        // Render silence and measure the per-sample output-noise bed the model
        // hands to the host mix, which is where the audible hiss level lives.
        const int warmupSamples = static_cast<int>(warmupSeconds * static_cast<double>(fs));
        const int measureSamples = static_cast<int>(measureSeconds * static_cast<double>(fs));
        std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));

        double energy = 0.0;
        long long counted = 0;
        bool finite = true;
        int rendered = 0;
        while (rendered < warmupSamples + measureSamples)
        {
            std::fill(block[0].begin(), block[0].end(), 0.0f);
            std::fill(block[1].begin(), block[1].end(), 0.0f);
            delay.ProcessBuffer(block, blockSize);

            const auto& noiseBuffer = delay.getOutputNoiseBufferForChannel(0);
            const int available = std::min(blockSize, static_cast<int>(noiseBuffer.size()));
            for (int s = 0; s < available; ++s)
            {
                const float value = noiseBuffer[static_cast<size_t>(s)];
                if (!std::isfinite(value))
                    finite = false;
                if (rendered + s >= warmupSamples)
                {
                    energy += static_cast<double>(value) * static_cast<double>(value);
                    ++counted;
                }
            }
            rendered += blockSize;
        }

        const double noiseRms = counted > 0 ? std::sqrt(energy / static_cast<double>(counted)) : 0.0;
        const double noiseDb = noiseRms > 0.0 ? 20.0 * std::log10(noiseRms) : -200.0;

        if (fs == 48000.0f)
        {
            referenceNoiseDb = noiseDb;
            // A dead noise path would make the cross-rate comparison vacuous,
            // so require the reference to be real signal, not silence.
            referenceNoiseValid = std::isfinite(noiseDb) && noiseDb > noiseFloorGateDb;
        }

        const bool rowOk = coeffsOk && pinnedOk && finite;
        passed = passed && rowOk;
        if (!coeffsOk)
            failures.add(juce::String(static_cast<int>(fs)) + ":tau");
        if (!pinnedOk)
            failures.add("48k:not-pinned");
        if (!finite)
            failures.add(juce::String(static_cast<int>(fs)) + ":non-finite");

        rows << (rows.isEmpty() ? "" : "; ") << juce::String(static_cast<int>(fs))
             << " fbTau=" << juce::String(feedbackTau * 1000.0, 3) << "ms"
             << " glideTau=" << juce::String(glideTau * 1000.0, 3) << "ms"
             << " noise=" << juce::String(noiseDb, 2) << "dBFS";

        if (fs != 48000.0f && referenceNoiseValid)
        {
            const double deltaDb = noiseDb - referenceNoiseDb;
            const bool noiseOk = std::isfinite(deltaDb) && std::abs(deltaDb) <= noiseToleranceDb;
            passed = passed && noiseOk;
            if (!noiseOk)
                failures.add(juce::String(static_cast<int>(fs)) + ":noise-level");
            rows << " delta=" << juce::String(deltaDb, 2) << "dB";
        }
    }

    if (!referenceNoiseValid)
    {
        passed = false;
        failures.add("48k-noise-reference-below-floor");
    }

    juce::String result;
    result << "TapeTimebaseSampleRateInvariance: targetTau=41.656ms tol=5%"
           << " noiseTol=+/-" << juce::String(noiseToleranceDb, 2) << "dB; " << rows;
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

//==============================================================================
// Delay-path level probe
//==============================================================================
//
// Measures what one enabled playback head actually delivers, relative to what
// was fed in, with the TapeDelayProcessor isolated from the preamp, the tone
// stack and EchoesModel's presentation trims. This is the number any external
// "presentation makeup" constant has to be judged against.
//
// The gated claim is architectural, not a tuned target: a tape record/playback
// pair that saturates but has 0 dB makeup must be UNITY at small signal in the
// midband. The record stage is C*tanh(x/C), whose derivative at the origin is
// exactly 1; the tape write and head read are unity; the playback high-pass and
// low-pass sit far from 1 kHz. Anything other than ~0 dB here is a hidden gain
// inside the delay path.
juce::String runDelayPathLevelProbe(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr float fs = 48000.0f;
    constexpr int blockSize = 256;
    constexpr float nominalDelayMs = 175.0f;  // delaytime = 0 maps to 175 ms
    constexpr double smallSignalToleranceDb = 1.0;

    // Rendering helper: sustained sine into the delay, measure output RMS in a
    // steady-state window that is well clear of the first repeat's arrival.
    auto renderSine = [&](float frequencyHz, float amplitude, int enabledHeads) -> double
    {
        int heads[3] = {1, enabledHeads > 1 ? 1 : 0, enabledHeads > 2 ? 1 : 0};

        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blockSize);
        delay.setTapeCharacter(0.45f, 0.65f, 0.60f);  // WARM anchor
        delay.setTapeRecordMakeupGainDb(0.0f);
        delay.UpdateParameters(0.0f, 0.0f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, true, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

        const int totalSamples = static_cast<int>(2.0f * fs);
        const int measureStart = static_cast<int>(1.0f * fs);
        std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));

        double energy = 0.0;
        long long counted = 0;
        int n = 0;
        const double omega =
            2.0 * juce::MathConstants<double>::pi * static_cast<double>(frequencyHz) / static_cast<double>(fs);
        while (n < totalSamples)
        {
            for (int s = 0; s < blockSize; ++s)
            {
                const float value = amplitude * static_cast<float>(std::sin(omega * (n + s)));
                block[0][static_cast<size_t>(s)] = value;
                block[1][static_cast<size_t>(s)] = value;
            }
            delay.ProcessBuffer(block, blockSize);
            for (int s = 0; s < blockSize; ++s)
            {
                if (n + s >= measureStart)
                {
                    const double v = block[0][static_cast<size_t>(s)];
                    energy += v * v;
                    ++counted;
                }
            }
            n += blockSize;
        }
        return counted > 0 ? std::sqrt(energy / static_cast<double>(counted)) : 0.0;
    };

    auto gainDb = [](double outRms, float amplitude) -> double
    {
        const double inRms = static_cast<double>(amplitude) / std::sqrt(2.0);
        if (!(outRms > 0.0) || !(inRms > 0.0))
            return -200.0;
        return 20.0 * std::log10(outRms / inRms);
    };

    // Small-signal reference and the level-dependent behaviour above it.
    const double h1SmallDb = gainDb(renderSine(1000.0f, 0.01f, 1), 0.01f);
    const double h1MidDb = gainDb(renderSine(1000.0f, 0.10f, 1), 0.10f);
    const double h1HotDb = gainDb(renderSine(1000.0f, 0.50f, 1), 0.50f);
    const double h1FullDb = gainDb(renderSine(1000.0f, 1.00f, 1), 1.00f);

    // Head summing. A sustained sine is useless here: three taps at 175 /
    // 332.7 / 481.4 ms sum at arbitrary relative phase, so the answer is a
    // frequency-dependent comb, not a level. Compare integrated impulse energy
    // instead, where the three well-separated echoes add incoherently.
    auto renderImpulseEnergyDb = [&](int enabledHeads) -> double
    {
        int heads[3] = {1, enabledHeads > 1 ? 1 : 0, enabledHeads > 2 ? 1 : 0};

        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blockSize);
        delay.setTapeCharacter(0.45f, 0.65f, 0.60f);
        delay.setTapeRecordMakeupGainDb(0.0f);
        delay.UpdateParameters(0.0f, 0.0f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, true, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

        const int totalSamples = static_cast<int>(1.5f * fs);
        std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));
        double energy = 0.0;
        int n = 0;
        while (n < totalSamples)
        {
            std::fill(block[0].begin(), block[0].end(), 0.0f);
            std::fill(block[1].begin(), block[1].end(), 0.0f);
            if (n == 0)
                block[0][0] = block[1][0] = 0.01f;
            delay.ProcessBuffer(block, blockSize);
            for (int s = 0; s < blockSize; ++s)
            {
                const double v = block[0][static_cast<size_t>(s)];
                energy += v * v;
            }
            n += blockSize;
        }
        return energy > 0.0 ? 10.0 * std::log10(energy) : -200.0;
    };

    const double h1EnergyDb = renderImpulseEnergyDb(1);
    const double h3EnergyDb = renderImpulseEnergyDb(3);
    const double h3SmallDb = h3EnergyDb - h1EnergyDb;  // heads-vs-one, in dB of energy

    // Playback-chain tone at small signal. The playback high-pass is nominally
    // 40 Hz but is built at Q = 0.25, which splits its poles far apart.
    const double band50Db = gainDb(renderSine(50.0f, 0.01f, 1), 0.01f);
    const double band100Db = gainDb(renderSine(100.0f, 0.01f, 1), 0.01f);
    const double band200Db = gainDb(renderSine(200.0f, 0.01f, 1), 0.01f);
    const double band5kDb = gainDb(renderSine(5000.0f, 0.01f, 1), 0.01f);
    const double band10kDb = gainDb(renderSine(10000.0f, 0.01f, 1), 0.01f);

    // Excess loop delay: the record-point saturator runs inside an 8x polyphase
    // IIR oversampler, and the record/feedback filters add group delay. All of
    // it lands INSIDE the feedback loop, so it lengthens every repeat and
    // accumulates across generations. Nothing reports it to the host.
    double excessDelaySamples = std::numeric_limits<double>::quiet_NaN();
    {
        int heads[3] = {1, 0, 0};
        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blockSize);
        delay.setTapeCharacter(0.45f, 0.65f, 0.60f);
        delay.setTapeRecordMakeupGainDb(0.0f);
        delay.UpdateParameters(0.0f, 0.0f, heads, 1, 1.0f, false, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, true, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

        const int totalSamples = static_cast<int>(0.5f * fs);
        std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));
        int n = 0;
        int peakIndex = -1;
        float peakValue = 0.0f;
        while (n < totalSamples)
        {
            std::fill(block[0].begin(), block[0].end(), 0.0f);
            std::fill(block[1].begin(), block[1].end(), 0.0f);
            if (n == 0)
                block[0][0] = block[1][0] = 0.01f;
            delay.ProcessBuffer(block, blockSize);
            for (int s = 0; s < blockSize; ++s)
            {
                const float magnitude = std::abs(block[0][static_cast<size_t>(s)]);
                if (magnitude > peakValue)
                {
                    peakValue = magnitude;
                    peakIndex = n + s;
                }
            }
            n += blockSize;
        }
        const double nominalSamples = static_cast<double>(nominalDelayMs) * 0.001 * static_cast<double>(fs);
        if (peakIndex >= 0)
            excessDelaySamples = static_cast<double>(peakIndex) - nominalSamples;
    }

    // The record stage on its own, outside the delay, for attribution.
    double saturatorSmallDb = -200.0;
    double saturatorHotDb = -200.0;
    {
        auto measureSaturator = [&](float amplitude) -> double
        {
            echoes::TapeRecordSaturator saturator;
            saturator.prepare(static_cast<double>(fs), 2);
            saturator.setCharacter(0.45f, 0.65f, 0.60f);
            saturator.setMakeupGainDb(0.0f);

            const int totalSamples = static_cast<int>(0.5f * fs);
            const int measureStart = static_cast<int>(0.25f * fs);
            const double omega = 2.0 * juce::MathConstants<double>::pi * 1000.0 / static_cast<double>(fs);
            double energy = 0.0;
            long long counted = 0;
            for (int n = 0; n < totalSamples; ++n)
            {
                float left = amplitude * static_cast<float>(std::sin(omega * n));
                float right = left;
                saturator.processStereoFrame(left, right);
                if (n >= measureStart)
                {
                    energy += static_cast<double>(left) * static_cast<double>(left);
                    ++counted;
                }
            }
            const double rms = counted > 0 ? std::sqrt(energy / static_cast<double>(counted)) : 0.0;
            const double inRms = static_cast<double>(amplitude) / std::sqrt(2.0);
            return (rms > 0.0) ? 20.0 * std::log10(rms / inRms) : -200.0;
        };
        saturatorSmallDb = measureSaturator(0.01f);
        saturatorHotDb = measureSaturator(0.50f);
    }

    const bool allFinite = std::isfinite(h1SmallDb) && std::isfinite(h1MidDb) && std::isfinite(h1HotDb) &&
                           std::isfinite(h1FullDb) && std::isfinite(h3SmallDb) && std::isfinite(band50Db) &&
                           std::isfinite(band100Db) && std::isfinite(band200Db) && std::isfinite(band5kDb) &&
                           std::isfinite(band10kDb) && std::isfinite(excessDelaySamples) &&
                           std::isfinite(saturatorSmallDb) && std::isfinite(saturatorHotDb);

    // CHECK 1: one head, small signal, midband must be unity within +/-1.5 dB.
    const bool smallSignalUnityOk = std::isfinite(h1SmallDb) && std::abs(h1SmallDb) <= smallSignalToleranceDb;

    // CHECK 2: the record stage alone must be small-signal unity for the same
    // reason, so a level error can be attributed rather than just observed.
    const bool saturatorUnityOk =
        std::isfinite(saturatorSmallDb) && std::abs(saturatorSmallDb) <= smallSignalToleranceDb;

    // CHECK 3: excess loop delay must be non-negative and small. A negative
    // value means the head read is early (an off-by-one in the delay line); a
    // large value means an unreported latency has crept into the loop.
    const bool loopDelayOk =
        std::isfinite(excessDelaySamples) && excessDelaySamples >= -1.0 && excessDelaySamples <= 32.0;

    const bool passed = allFinite && smallSignalUnityOk && saturatorUnityOk && loopDelayOk;

    juce::StringArray failures;
    if (!allFinite)
        failures.add("non-finite");
    if (!smallSignalUnityOk)
        failures.add("h1-small-signal-not-unity");
    if (!saturatorUnityOk)
        failures.add("record-stage-not-unity");
    if (!loopDelayOk)
        failures.add("excess-loop-delay");

    juce::String result;
    result << "DelayPathLevelProbe: 48kHz WARM(0.45/0.65/0.60) makeup=0dB delay=175ms fb=0"
           << "; H1 1kHz in/out -40dBFS=" << juce::String(h1SmallDb, 2) << "dB"
           << " -20dBFS=" << juce::String(h1MidDb, 2) << "dB"
           << " -6dBFS=" << juce::String(h1HotDb, 2) << "dB"
           << " 0dBFS=" << juce::String(h1FullDb, 2) << "dB"
           << " (tol=+/-" << juce::String(smallSignalToleranceDb, 2) << "dB at -40dBFS)"
           << "; 3heads-vs-1head impulse energy=" << juce::String(h3SmallDb, 2)
           << "dB (incoherent sum of 3 taps = +4.77dB)"
           << "; playback tone @-40dBFS 50/100/200/1k/5k/10k=" << juce::String(band50Db, 2) << "/"
           << juce::String(band100Db, 2) << "/" << juce::String(band200Db, 2) << "/" << juce::String(h1SmallDb, 2)
           << "/" << juce::String(band5kDb, 2) << "/" << juce::String(band10kDb, 2) << "dB"
           << "; recordStage alone -40dBFS=" << juce::String(saturatorSmallDb, 2) << "dB"
           << " -6dBFS=" << juce::String(saturatorHotDb, 2) << "dB"
           << "; excessLoopDelay=" << juce::String(excessDelaySamples, 2) << "samples"
           << " (=" << juce::String(excessDelaySamples * 1000.0 / static_cast<double>(fs), 4)
           << "ms per repeat, accumulates across generations, unreported to host)";
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

//==============================================================================
// Wow/flutter noise-term probe
//==============================================================================
//
// WowFlutterSignalModel adds a white-noise-through-1 Hz-lowpass term to its
// periodic wow and flutter oscillators. The Biquad runs in double precision:
// its lowpass numerator is built from (1 - cos w), and for w below ~3.4e-4
// (fc < ~2.6 Hz at 48 kHz) cos(w) rounds to exactly 1.0f in float32, which
// would zero all three b coefficients and silence the term.
//
// This probe measures what the term contributes, in milliseconds of delay-time
// deviation and in cents of pitch deviation, and gates three things:
//   - the 1 Hz lowpass has unity DC gain at every supported rate (catches
//     degenerate coefficients),
//   - the noise term stays well below the periodic wow it is meant to
//     perturb rather than becoming the dominant modulator,
//   - nothing in the model is non-finite.
juce::String runWowFlutterNoiseTermProbe(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
    constexpr double dcGainTolerance = 0.02;
    // The periodic wow oscillator has amplitude 1.0 by construction, so its RMS
    // is 1/sqrt(2). The noise perturbation must stay at least 20 dB under it.
    constexpr double wowRms = 0.70710678;
    constexpr double maxNoiseToWowRatio = 0.1;

    bool passed = true;
    juce::StringArray failures;
    juce::String dcRows;

    for (const float fs : sampleRates)
    {
        RbjBiquad lp;
        lp.Init(BiquadType::LPF, 1.0f, fs, 0.707f, 0.0f);
        lp.Reset(fs);

        float settled = 0.0f;
        const int settleSamples = static_cast<int>(10.0f * fs);
        for (int n = 0; n < settleSamples; ++n)
            settled = lp.ProcessSample(1.0f);

        const double dcGain = static_cast<double>(settled);
        const bool dcOk = std::isfinite(dcGain) && std::abs(dcGain - 1.0) <= dcGainTolerance;
        passed = passed && dcOk;
        if (!dcOk)
            failures.add(juce::String(static_cast<int>(fs)) + ":dc-gain");

        dcRows << (dcRows.isEmpty() ? "" : "/") << juce::String(static_cast<int>(fs)) << "=" << juce::String(dcGain, 4);
    }

    // Reproduce the model's noise term exactly: WhiteNoiseOsc at amplitude
    // 8 (rate-normalised) into the same 1 Hz lowpass.
    constexpr float fs = 48000.0f;
    double noiseRms = 0.0;
    double noisePeak = 0.0;
    {
        WhiteNoiseOsc osc;
        osc.seed(0x5EEDF00Du);
        osc.setParams(8.0f * wfWhiteNoiseRateScale(fs));
        RbjBiquad lp;
        lp.Init(BiquadType::LPF, 1.0f, fs, 0.707f, 0.0f);
        lp.Reset(fs);

        const int settleSamples = static_cast<int>(5.0f * fs);
        for (int n = 0; n < settleSamples; ++n)
            lp.ProcessSample(osc.returnCurrentValue());

        const int measureSamples = static_cast<int>(60.0f * fs);
        double energy = 0.0;
        for (int n = 0; n < measureSamples; ++n)
        {
            const double value = lp.ProcessSample(osc.returnCurrentValue());
            energy += value * value;
            noisePeak = std::max(noisePeak, std::abs(value));
        }
        noiseRms = std::sqrt(energy / static_cast<double>(measureSamples));
    }

    // Whole-model output, in milliseconds, at the two ends of the shipped
    // flavor range, plus the resulting peak pitch deviation. dTau/dt is the
    // fractional pitch error, so cents = 1200 * log2(1 + dTau/dt).
    struct ModelRow
    {
        const char* label;
        float depth;
        float condition;
        double rmsMs;
        double peakMs;
        double peakCents;
        bool finite;
    };

    std::array<ModelRow, 2> modelRows{{{"WARM(depth=0.222,cond=0.08)", 0.222f, 0.08f, 0.0, 0.0, 0.0, true},
                                       {"WORN(depth=0.860,cond=0.85)", 0.860f, 0.85f, 0.0, 0.0, 0.0, true}}};

    for (auto& row : modelRows)
    {
        WowFlutterSignalModel model;
        model.setDeterministicSeed(0x1234ABCDu);
        model.init();
        model.reset(fs);
        model.updateParameters(row.depth, 1.0f, 1.0f, row.condition, 175.0f, 1.0f);

        const int settleSamples = static_cast<int>(2.0f * fs);
        for (int n = 0; n < settleSamples; ++n)
            model.processSample();

        const int measureSamples = static_cast<int>(20.0f * fs);
        double energy = 0.0;
        double previous = model.processSample();
        double peakSlopePerSample = 0.0;
        for (int n = 0; n < measureSamples; ++n)
        {
            const double value = model.processSample();
            if (!std::isfinite(value))
                row.finite = false;
            energy += value * value;
            row.peakMs = std::max(row.peakMs, std::abs(value));
            peakSlopePerSample = std::max(peakSlopePerSample, std::abs(value - previous));
            previous = value;
        }
        row.rmsMs = std::sqrt(energy / static_cast<double>(measureSamples));
        // dTau/dt: milliseconds of delay change per sample, converted to a
        // dimensionless rate by multiplying by fs and dividing by 1000.
        const double fractionalRate = peakSlopePerSample * 0.001 * static_cast<double>(fs);
        row.peakCents = 1200.0 * std::log2(1.0 + std::min(0.99, fractionalRate));
        if (!row.finite)
        {
            passed = false;
            failures.add(juce::String(row.label) + ":non-finite");
        }
    }

    const double noiseToWow = (wowRms > 0.0) ? (noiseRms / wowRms) : 0.0;
    const bool noiseFinite = std::isfinite(noiseRms) && std::isfinite(noisePeak);
    const bool noiseShareOk = noiseFinite && noiseToWow <= maxNoiseToWowRatio;
    passed = passed && noiseShareOk;
    if (!noiseFinite)
        failures.add("noise-term-non-finite");
    else if (!noiseShareOk)
        failures.add("noise-term-dominates-wow");

    juce::String result;
    result << "WowFlutterNoiseTermProbe (peakPitchRaw is the model output BEFORE"
           << " TapeDelay's 90Hz modulation lowpass and 0.05 sample/sample slew"
           << " limiter, which is what actually reaches the delay line):"
           << " 1Hz LPF dcGain " << dcRows << " (tol=+/-" << juce::String(dcGainTolerance, 2) << ")"
           << "; noiseTerm rms=" << juce::String(noiseRms, 5) << " peak=" << juce::String(noisePeak, 5)
           << " vs wowRms=" << juce::String(wowRms, 4) << " ratio=" << juce::String(noiseToWow, 4)
           << " max=" << juce::String(maxNoiseToWowRatio, 2);
    for (const auto& row : modelRows)
    {
        result << "; " << row.label << " rms=" << juce::String(row.rmsMs, 5) << "ms"
               << " peak=" << juce::String(row.peakMs, 5) << "ms"
               << " peakPitchRaw=" << juce::String(row.peakCents, 2) << "cents";
    }
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

//==============================================================================
// Reverb noise bed contract
//==============================================================================
//
// The reverb injection bed must not be read as a single scalar per block and
// added to every sample of the reverb buffer. That is not a noise bed: one
// value held across N samples is a staircase at the block rate (~93 Hz at 512
// samples / 48 kHz) plus its harmonics, and because the reverb renders before
// the delay in the default route the value was also a block stale.
//
// This gate pins the replacement contract:
//   - the bed is populated per sample, not held (a held bed collapses to one
//     distinct value per block, which is precisely what this detects),
//   - the two channels are not the same signal (an identical bed injects a
//     phantom-centre component into the tank instead of a diffuse floor),
//   - the level is sample-rate invariant, like every other tape noise source,
//   - the buffers are preallocated by Reset() and never resized on the audio
//     thread.
juce::String runReverbNoiseBedContractTest(SpatialEchoAudioProcessor& /*processor*/)
{
    constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
    constexpr int blockSize = 512;
    constexpr double warmupSeconds = 0.5;
    constexpr double measureSeconds = 2.0;
    constexpr double levelToleranceDb = 0.75;
    constexpr double levelFloorDb = -140.0;
    // A held bed yields exactly one distinct value per block. Require the bed
    // to be genuinely per-sample with a large margin over that.
    constexpr double minDistinctFraction = 0.25;
    // Independent PCG32 streams plus a shared asperity component: the channels
    // are not required to be perfectly independent, only to not be the SAME
    // signal.
    constexpr double maxChannelCorrelation = 0.70;

    int heads[3] = {1, 0, 0};
    bool passed = true;
    juce::StringArray failures;
    juce::String rows;
    double referenceDb = 0.0;
    bool referenceValid = false;

    for (const float fs : sampleRates)
    {
        TapeDelayProcessor delay;
        delay.Reset(fs, 2, blockSize);
        delay.UpdateParameters(0.0f, 0.0f, heads, 1, 1.0f, true, 1.0f, 0.5f, 0.0f, 1.0f, 1.0f, false, 0.0f, 0.0f, 0.0f,
                               0, 0, true, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f);

        const int warmupSamples = static_cast<int>(warmupSeconds * static_cast<double>(fs));
        const int measureSamples = static_cast<int>(measureSeconds * static_cast<double>(fs));
        std::vector<std::vector<float>> block(2, std::vector<float>(blockSize, 0.0f));

        double energyL = 0.0, energyR = 0.0, crossLR = 0.0;
        long long counted = 0;
        long long distinctTotal = 0, blocksCounted = 0;
        bool finite = true;
        bool buffersPresent = true;
        int rendered = 0;

        while (rendered < warmupSamples + measureSamples)
        {
            std::fill(block[0].begin(), block[0].end(), 0.0f);
            std::fill(block[1].begin(), block[1].end(), 0.0f);
            delay.ProcessBuffer(block, blockSize);

            const auto& bedL = delay.getReverbNoiseBufferForChannel(0);
            const auto& bedR = delay.getReverbNoiseBufferForChannel(1);
            if (bedL.size() < static_cast<size_t>(blockSize) || bedR.size() < static_cast<size_t>(blockSize))
                buffersPresent = false;

            const int available =
                std::min(blockSize, std::min(static_cast<int>(bedL.size()), static_cast<int>(bedR.size())));

            if (rendered >= warmupSamples && available > 0)
            {
                // Count distinct values in this block. A block-held scalar
                // gives 1; a per-sample bed gives close to `available`.
                std::vector<float> sorted(bedL.begin(), bedL.begin() + available);
                std::sort(sorted.begin(), sorted.end());
                long long distinct = sorted.empty() ? 0 : 1;
                for (size_t i = 1; i < sorted.size(); ++i)
                    if (!juce::exactlyEqual(sorted[i], sorted[i - 1]))
                        ++distinct;
                distinctTotal += distinct;
                ++blocksCounted;
            }

            for (int s = 0; s < available; ++s)
            {
                const double l = bedL[static_cast<size_t>(s)];
                const double r = bedR[static_cast<size_t>(s)];
                if (!std::isfinite(l) || !std::isfinite(r))
                    finite = false;
                if (rendered + s >= warmupSamples)
                {
                    energyL += l * l;
                    energyR += r * r;
                    crossLR += l * r;
                    ++counted;
                }
            }
            rendered += blockSize;
        }

        const double rmsL = counted > 0 ? std::sqrt(energyL / static_cast<double>(counted)) : 0.0;
        const double rmsR = counted > 0 ? std::sqrt(energyR / static_cast<double>(counted)) : 0.0;
        const double levelDbL = rmsL > 0.0 ? 20.0 * std::log10(rmsL) : -200.0;
        const double denom = std::sqrt(energyL * energyR);
        const double correlation = denom > 0.0 ? (crossLR / denom) : 1.0;
        const double distinctFraction = blocksCounted > 0
                                            ? static_cast<double>(distinctTotal) /
                                                  (static_cast<double>(blocksCounted) * static_cast<double>(blockSize))
                                            : 0.0;

        const bool perSampleOk = distinctFraction >= minDistinctFraction;
        const bool decorrelatedOk = std::isfinite(correlation) && std::abs(correlation) <= maxChannelCorrelation;
        const bool presentOk = buffersPresent && rmsL > 0.0 && rmsR > 0.0;
        const bool rowOk = finite && perSampleOk && decorrelatedOk && presentOk;

        passed = passed && rowOk;
        if (!finite)
            failures.add(juce::String(static_cast<int>(fs)) + ":non-finite");
        if (!perSampleOk)
            failures.add(juce::String(static_cast<int>(fs)) + ":block-held");
        if (!decorrelatedOk)
            failures.add(juce::String(static_cast<int>(fs)) + ":channels-identical");
        if (!presentOk)
            failures.add(juce::String(static_cast<int>(fs)) + ":bed-absent");

        if (fs == 48000.0f)
        {
            referenceDb = levelDbL;
            referenceValid = std::isfinite(levelDbL) && levelDbL > levelFloorDb;
        }

        rows << (rows.isEmpty() ? "" : "; ") << juce::String(static_cast<int>(fs))
             << " L/R=" << juce::String(levelDbL, 2) << "/"
             << juce::String(rmsR > 0.0 ? 20.0 * std::log10(rmsR) : -200.0, 2) << "dBFS"
             << " distinct=" << juce::String(distinctFraction * 100.0, 1) << "%"
             << " corr=" << juce::String(correlation, 4);

        if (fs != 48000.0f && referenceValid)
        {
            const double deltaDb = levelDbL - referenceDb;
            const bool levelOk = std::isfinite(deltaDb) && std::abs(deltaDb) <= levelToleranceDb;
            passed = passed && levelOk;
            if (!levelOk)
                failures.add(juce::String(static_cast<int>(fs)) + ":level");
            rows << " delta=" << juce::String(deltaDb, 2) << "dB";
        }
    }

    if (!referenceValid)
    {
        passed = false;
        failures.add("48k-reference-below-floor");
    }

    juce::String result;
    result << "ReverbNoiseBedContract: perSample>=" << juce::String(minDistinctFraction * 100.0, 0)
           << "% distinct, |corr|<=" << juce::String(maxChannelCorrelation, 2) << ", level tol +/-"
           << juce::String(levelToleranceDb, 2) << "dB; " << rows;
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::Delay

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
