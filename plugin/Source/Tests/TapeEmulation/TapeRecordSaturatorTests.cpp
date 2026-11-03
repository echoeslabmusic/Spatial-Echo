// TapeRecordSaturatorTests: Validates boundedness, memory, reset, silence decay, channel independence, small-signal
//   gain, hot-flux ceiling, sample-rate normalization, and non-finite recovery.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "TapeRecordSaturatorTests.h"
#include "../../DSP/TapeRecordSaturator.h"
#include "../../DSP/TapeFlavors.h"
#include "../Common/TestUtilities.h"
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace SpatialEchoTests::TapeEmulation
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

struct BehaviorCheck
{
    juce::String name;
    bool passed = false;
    juce::String details;
};

float rms(const std::vector<float>& samples)
{
    return TestUtilities::rmsAbs(samples);
}

float rmsWindow(const std::vector<float>& samples, int start, int count)
{
    if (samples.empty() || count <= 0)
        return 0.0f;

    const int clampedStart = juce::jlimit(0, static_cast<int>(samples.size()) - 1, start);
    const int clampedEnd = juce::jlimit(clampedStart, static_cast<int>(samples.size()), clampedStart + count);

    double sumSquares = 0.0;
    int used = 0;
    for (int i = clampedStart; i < clampedEnd; ++i)
    {
        const double sample = static_cast<double>(samples[static_cast<size_t>(i)]);
        sumSquares += sample * sample;
        ++used;
    }

    return used > 0 ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(used))) : 0.0f;
}

float maxAbs(const std::vector<float>& samples)
{
    return TestUtilities::peakAbs(samples);
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    const int count = static_cast<int>(std::min(a.size(), b.size()));
    float peak = 0.0f;
    for (int i = 0; i < count; ++i)
        peak = std::max(peak, std::abs(a[static_cast<size_t>(i)] - b[static_cast<size_t>(i)]));
    return peak;
}

BehaviorCheck checkFiniteAndBounds()
{
    echoes::TapeRecordSaturator saturator;
    saturator.prepare(48000.0, 2);
    saturator.setCharacter(1.0f, 1.0f, 1.0f);

    constexpr std::array<float, 8> stimulus = {-1000.0f, -8.0f, -1.0f, -0.25f, 0.25f, 1.0f, 8.0f, 1000.0f};
    std::vector<float> outputs;
    outputs.reserve(stimulus.size() * 2);

    bool finite = true;
    for (float sample : stimulus)
    {
        float left = sample;
        float right = -sample;
        saturator.processStereoFrame(left, right);
        outputs.push_back(left);
        outputs.push_back(right);
        finite = finite && std::isfinite(left) && std::isfinite(right);
    }

    const float peak = maxAbs(outputs);
    const bool bounded = peak <= 1.5f;

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorFiniteBounds";
    check.passed = finite && bounded;
    check.details = "peak=" + juce::String(peak, 6) + " finite=" + juce::String(finite ? "true" : "false");
    return check;
}

BehaviorCheck checkHistoryDependence()
{
    echoes::TapeRecordSaturator primed;
    echoes::TapeRecordSaturator fresh;
    primed.prepare(48000.0, 1);
    fresh.prepare(48000.0, 1);
    primed.setCharacter(0.85f, 0.9f, 0.6f);
    fresh.setCharacter(0.85f, 0.9f, 0.6f);

    for (int i = 0; i < 96; ++i)
        primed.processMono(i == 0 ? 0.95f : 0.0f, 0);

    const float probeInput = 0.18f;
    const float primedOutput = primed.processMono(probeInput, 0);
    const float freshOutput = fresh.processMono(probeInput, 0);
    const float delta = std::abs(primedOutput - freshOutput);

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorHistoryDependence";
    check.passed = std::isfinite(primedOutput) && std::isfinite(freshOutput) && delta > 1.0e-4f;
    check.details = "primed=" + juce::String(primedOutput, 6) + " fresh=" + juce::String(freshOutput, 6) +
                    " delta=" + juce::String(delta, 6);
    return check;
}

BehaviorCheck checkResetIdentity()
{
    echoes::TapeRecordSaturator saturator;
    saturator.prepare(48000.0, 1);
    saturator.setCharacter(0.7f, 0.8f, 0.4f);

    std::vector<float> firstPass;
    std::vector<float> secondPass;
    firstPass.reserve(1024);
    secondPass.reserve(1024);

    for (int i = 0; i < 1024; ++i)
    {
        const float sample =
            0.42f * std::sin(static_cast<float>(2.0 * kPi * 330.0 * i / 48000.0)) + ((i % 17 == 0) ? 0.08f : 0.0f);
        firstPass.push_back(saturator.processMono(sample, 0));
    }

    saturator.reset();

    for (int i = 0; i < 1024; ++i)
    {
        const float sample =
            0.42f * std::sin(static_cast<float>(2.0 * kPi * 330.0 * i / 48000.0)) + ((i % 17 == 0) ? 0.08f : 0.0f);
        secondPass.push_back(saturator.processMono(sample, 0));
    }

    const float error = maxDiff(firstPass, secondPass);

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorResetIdentity";
    check.passed = error <= 1.0e-7f;
    check.details = "maxDiff=" + juce::String(error, 9);
    return check;
}

BehaviorCheck checkSilenceDecay()
{
    echoes::TapeRecordSaturator saturator;
    saturator.prepare(48000.0, 1);
    saturator.setCharacter(1.0f, 1.0f, 0.75f);

    for (int i = 0; i < 192; ++i)
    {
        const float sign = (i & 1) == 0 ? 1.0f : -1.0f;
        const float excitation = sign * (0.55f + 0.35f * std::sin(static_cast<float>(0.19 * i)));
        saturator.processMono(excitation, 0);
    }

    std::vector<float> tail;
    tail.reserve(4096);
    for (int i = 0; i < 4096; ++i)
        tail.push_back(saturator.processMono(0.0f, 0));

    const float earlyRms = rmsWindow(tail, 0, 512);
    const float lateRms = rmsWindow(tail, static_cast<int>(tail.size()) - 512, 512);

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorSilenceDecay";
    check.passed = earlyRms > 1.0e-5f && lateRms < 1.0e-4f && lateRms < earlyRms * 0.2f;
    check.details = "earlyRms=" + juce::String(earlyRms, 8) + " lateRms=" + juce::String(lateRms, 8);
    return check;
}

BehaviorCheck checkStereoIndependence()
{
    echoes::TapeRecordSaturator withBusyRight;
    echoes::TapeRecordSaturator withSilentRight;
    withBusyRight.prepare(48000.0, 2);
    withSilentRight.prepare(48000.0, 2);
    withBusyRight.setCharacter(0.8f, 0.85f, 0.55f);
    withSilentRight.setCharacter(0.8f, 0.85f, 0.55f);

    std::vector<float> leftBusy;
    std::vector<float> leftSilent;
    leftBusy.reserve(1024);
    leftSilent.reserve(1024);

    for (int i = 0; i < 1024; ++i)
    {
        float leftA = 0.3f * std::sin(static_cast<float>(2.0 * kPi * 330.0 * i / 48000.0));
        float rightA = 0.92f * std::sin(static_cast<float>(2.0 * kPi * 1110.0 * i / 48000.0));
        float leftB = leftA;
        float rightB = 0.0f;

        withBusyRight.processStereoFrame(leftA, rightA);
        withSilentRight.processStereoFrame(leftB, rightB);
        leftBusy.push_back(leftA);
        leftSilent.push_back(leftB);
    }

    const float delta = maxDiff(leftBusy, leftSilent);

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorStereoIndependence";
    check.passed = delta <= 1.0e-7f;
    check.details = "maxLeftDelta=" + juce::String(delta, 9);
    return check;
}

BehaviorCheck checkSmallSignalUnity()
{
    echoes::TapeRecordSaturator saturator;
    saturator.prepare(48000.0, 2);
    saturator.setCharacter(1.0f, 1.0f, 1.0f);

    constexpr int totalFrames = 4096;
    constexpr int warmupFrames = 512;
    std::vector<float> inputs;
    std::vector<float> outputs;
    inputs.reserve(totalFrames - warmupFrames);
    outputs.reserve(totalFrames - warmupFrames);

    for (int i = 0; i < totalFrames; ++i)
    {
        float left = 1.0e-4f * std::sin(static_cast<float>(2.0 * kPi * 997.0 * i / 48000.0));
        float right = left;
        saturator.processStereoFrame(left, right);

        if (i >= warmupFrames)
        {
            inputs.push_back(1.0e-4f * std::sin(static_cast<float>(2.0 * kPi * 997.0 * i / 48000.0)));
            outputs.push_back(left);
        }
    }

    const float inRms = rms(inputs);
    const float outRms = rms(outputs);
    const float gain = outRms / std::max(inRms, 1.0e-12f);

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorSmallSignalUnity";
    check.passed = gain >= 0.98f && gain <= 1.02f;
    check.details =
        "gain=" + juce::String(gain, 6) + " inRms=" + juce::String(inRms, 9) + " outRms=" + juce::String(outRms, 9);
    return check;
}

BehaviorCheck checkHotFluxCeiling()
{
    auto renderSteady = [](float input)
    {
        echoes::TapeRecordSaturator saturator;
        saturator.prepare(48000.0, 1);
        saturator.setCharacter(1.0f, 1.0f, 1.0f);

        float output = 0.0f;
        for (int frame = 0; frame < 4096; ++frame)
            output = saturator.processMono(input, 0);
        return output;
    };

    const float positive = renderSteady(8.0f);
    const float negative = renderSteady(-8.0f);
    const float positiveMagnitude = std::abs(positive);
    const float negativeMagnitude = std::abs(negative);
    const bool finite = std::isfinite(positive) && std::isfinite(negative);
    const bool bounded = positiveMagnitude <= 0.705f && negativeMagnitude <= 0.705f;
    const bool reachesCeiling = positiveMagnitude >= 0.68f && negativeMagnitude >= 0.68f;

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorHotFluxCeiling";
    check.passed = finite && bounded && reachesCeiling;
    check.details = "steady(+/-)=" + juce::String(positive, 6) + "/" + juce::String(negative, 6) +
                    " bounded=" + juce::String(bounded ? "true" : "false");
    return check;
}

struct BurstMetrics
{
    float fullRms = 0.0f;
    float lateRms = 0.0f;
    float peak = 0.0f;
};

BurstMetrics renderBurstMetrics(double sampleRate)
{
    echoes::TapeRecordSaturator saturator;
    saturator.prepare(sampleRate, 1);
    saturator.setCharacter(0.9f, 0.85f, 0.65f);

    const int totalSamples = static_cast<int>(std::round(sampleRate * 0.06));
    const int burstSamples = static_cast<int>(std::round(sampleRate * 0.02));
    const int lateWindow = static_cast<int>(std::round(sampleRate * 0.01));

    std::vector<float> rendered;
    rendered.reserve(static_cast<size_t>(totalSamples));

    for (int i = 0; i < totalSamples; ++i)
    {
        float input = 0.0f;
        if (i < burstSamples)
            input = 0.65f * std::sin(static_cast<float>(2.0 * kPi * 440.0 * i / sampleRate));

        rendered.push_back(saturator.processMono(input, 0));
    }

    BurstMetrics metrics;
    metrics.fullRms = rms(rendered);
    metrics.lateRms = rmsWindow(rendered, totalSamples - lateWindow, lateWindow);
    metrics.peak = maxAbs(rendered);
    return metrics;
}

BehaviorCheck checkSampleRateConsistency()
{
    const BurstMetrics lowRate = renderBurstMetrics(44100.0);
    const BurstMetrics highRate = renderBurstMetrics(96000.0);

    const float fullRatio = lowRate.fullRms / std::max(highRate.fullRms, 1.0e-12f);
    const float lateRatio = lowRate.lateRms / std::max(highRate.lateRms, 1.0e-12f);
    const float peakRatio = lowRate.peak / std::max(highRate.peak, 1.0e-12f);

    const bool fullOk = fullRatio >= 0.85f && fullRatio <= 1.15f;
    const bool lateOk = lateRatio >= 0.75f && lateRatio <= 1.25f;
    const bool peakOk = peakRatio >= 0.85f && peakRatio <= 1.15f;

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorSampleRateConsistency";
    check.passed = fullOk && lateOk && peakOk;
    check.details = "fullRatio=" + juce::String(fullRatio, 6) + " lateRatio=" + juce::String(lateRatio, 6) +
                    " peakRatio=" + juce::String(peakRatio, 6);
    return check;
}

BehaviorCheck checkNonFiniteRecovery()
{
    echoes::TapeRecordSaturator contaminated;
    echoes::TapeRecordSaturator clean;
    contaminated.prepare(48000.0, 1);
    clean.prepare(48000.0, 1);
    contaminated.setCharacter(0.9f, 1.0f, 0.7f);
    clean.setCharacter(0.9f, 1.0f, 0.7f);

    contaminated.setCharacter(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity());

    const std::array<float, 3> contaminatedOutputs = {
        contaminated.processMono(std::numeric_limits<float>::quiet_NaN(), 0),
        contaminated.processMono(std::numeric_limits<float>::infinity(), 0),
        contaminated.processMono(-std::numeric_limits<float>::infinity(), 0)};
    const float immediateFiniteOutput = contaminated.processMono(0.0f, 0);

    // Invalid samples are defined to enter the oversampler as silence. Keep
    // the clean comparator on the same sample clock so this check isolates
    // recovery instead of comparing different filter phases.
    clean.processMono(0.0f, 0);
    clean.processMono(0.0f, 0);
    clean.processMono(0.0f, 0);
    clean.processMono(0.0f, 0);

    bool contaminatedFramesAreFinite = std::isfinite(immediateFiniteOutput);
    for (float output : contaminatedOutputs)
        contaminatedFramesAreFinite = contaminatedFramesAreFinite && std::isfinite(output);

    contaminated.setCharacter(0.9f, 1.0f, 0.7f);

    for (int i = 0; i < 16; ++i)
        contaminated.processMono(0.0f, 0);

    for (int i = 0; i < 16; ++i)
        clean.processMono(0.0f, 0);

    const float contaminatedProbe = contaminated.processMono(0.23f, 0);
    const float cleanProbe = clean.processMono(0.23f, 0);
    const float delta = std::abs(contaminatedProbe - cleanProbe);

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorNonFiniteRecovery";
    check.passed = contaminatedFramesAreFinite && std::isfinite(contaminatedProbe) && std::isfinite(cleanProbe) &&
                   contaminatedProbe <= 1.5f && contaminatedProbe >= -1.5f && delta <= 1.0e-5f;
    check.details = "invalidFramesFinite=" + juce::String(contaminatedFramesAreFinite ? "true" : "false") +
                    " immediate=" + juce::String(immediateFiniteOutput, 7) +
                    " contaminated=" + juce::String(contaminatedProbe, 7) + " clean=" + juce::String(cleanProbe, 7) +
                    " delta=" + juce::String(delta, 9);
    return check;
}

BehaviorCheck checkSupportedSmoothingCallPatterns()
{
    echoes::TapeRecordSaturator mono;
    echoes::TapeRecordSaturator stereo;
    mono.prepare(48000.0, 1);
    stereo.prepare(48000.0, 2);
    mono.setCharacter(0.0f, 0.0f, 0.0f);
    stereo.setCharacter(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < 32; ++i)
    {
        mono.processMono(0.2f);
        float left = 0.2f;
        float right = 0.2f;
        stereo.processStereoFrame(left, right);
    }

    mono.setCharacter(1.0f, 1.0f, 0.75f);
    stereo.setCharacter(1.0f, 1.0f, 0.75f);

    float maxMonoStereoDelta = 0.0f;
    float maxStereoChannelDelta = 0.0f;
    for (int frame = 0; frame < 640; ++frame)
    {
        const float input = 0.35f * std::sin(static_cast<float>(2.0 * kPi * 733.0 * frame / 48000.0));
        const float monoOutput = mono.processMono(input);
        float left = input;
        float right = input;
        stereo.processStereoFrame(left, right);
        maxMonoStereoDelta = std::max(maxMonoStereoDelta, std::abs(monoOutput - left));
        maxStereoChannelDelta = std::max(maxStereoChannelDelta, std::abs(left - right));
    }

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorSmoothingCallPatterns";
    check.passed = maxMonoStereoDelta <= 1.0e-7f && maxStereoChannelDelta <= 1.0e-7f;
    check.details = "monoStereoDelta=" + juce::String(maxMonoStereoDelta, 9) +
                    " stereoChannelDelta=" + juce::String(maxStereoChannelDelta, 9);
    return check;
}
// Physically-motivated record-transfer contract.
//
// Magnetic recording is linear at low flux and compressive as the medium
// approaches saturation. The Jiles-Atherton anhysteretic law used by every
// serious tape model (M = M_s * L(Q), L = coth(Q) - 1/Q) expands as
// L(q) = q/3 - q^3/45 + ..., i.e. its third-order term is NEGATIVE. A record
// stage whose gain RISES with level is therefore unphysical, and inside a
// regeneration loop it is a positive-feedback mechanism: loop gain climbs as
// the tail grows. This check measures the fundamental gain of the record
// stage across level at every shipping flavour anchor and requires the curve
// to be non-expanding and monotonically non-increasing.
struct LevelPoint
{
    float inputDbfs = 0.0f;
    float gainDb = 0.0f;
    float thdPercent = 0.0f;
};

LevelPoint measureTransferPoint(float drive, float saturation, float bias, float inputDbfs, float makeupDb = 0.0f)
{
    constexpr double sampleRate = 48000.0;
    constexpr int captureSamples = 4096;
    constexpr double requestedHz = 1000.0;
    const int cycles = static_cast<int>(std::lround(requestedHz * captureSamples / sampleRate));
    const double actualHz = cycles * sampleRate / captureSamples;
    const double phaseIncrement = 2.0 * kPi * actualHz / sampleRate;
    const float amplitude = std::pow(10.0f, inputDbfs / 20.0f);

    echoes::TapeRecordSaturator saturator;
    saturator.prepare(sampleRate, 1);
    saturator.setCharacter(drive, saturation, bias);
    saturator.setMakeupGainDb(makeupDb);

    double phase = 0.0;
    const int warmupSamples = static_cast<int>(std::ceil(0.12 * sampleRate));
    for (int i = 0; i < warmupSamples; ++i)
    {
        saturator.processMono(amplitude * static_cast<float>(std::sin(phase)), 0);
        phase += phaseIncrement;
        if (phase >= 2.0 * kPi)
            phase -= 2.0 * kPi;
    }

    double sinSum = 0.0, cosSum = 0.0;
    std::array<double, 8> harmonicSin{};
    std::array<double, 8> harmonicCos{};
    for (int i = 0; i < captureSamples; ++i)
    {
        const double sine = std::sin(phase);
        const double output = saturator.processMono(amplitude * static_cast<float>(sine), 0);
        sinSum += output * sine;
        cosSum += output * std::cos(phase);
        for (int harmonic = 2; harmonic <= 9; ++harmonic)
        {
            const double harmonicPhase = phase * harmonic;
            harmonicSin[static_cast<size_t>(harmonic - 2)] += output * std::sin(harmonicPhase);
            harmonicCos[static_cast<size_t>(harmonic - 2)] += output * std::cos(harmonicPhase);
        }
        phase += phaseIncrement;
        if (phase >= 2.0 * kPi)
            phase -= 2.0 * kPi;
    }

    const double scale = 2.0 / captureSamples;
    const double fundamental = scale * std::hypot(sinSum, cosSum);
    double harmonicPower = 0.0;
    for (size_t index = 0; index < harmonicSin.size(); ++index)
    {
        const double magnitude = scale * std::hypot(harmonicSin[index], harmonicCos[index]);
        harmonicPower += magnitude * magnitude;
    }

    LevelPoint point;
    point.inputDbfs = inputDbfs;
    point.gainDb =
        20.0f * std::log10(static_cast<float>(std::max(1.0e-12, fundamental / std::max(1.0e-12f, amplitude))));
    point.thdPercent = 100.0f * static_cast<float>(std::sqrt(harmonicPower) / std::max(1.0e-12, fundamental));
    return point;
}

BehaviorCheck checkMonotoneCompression()
{
    constexpr std::array<float, 11> levels{-40.0f, -30.0f, -24.0f, -20.0f, -15.0f, -12.0f,
                                           -9.0f,  -6.0f,  -3.0f,  -1.5f,  0.0f};
    // Expansion is a defect, not a tolerance: allow only measurement noise.
    constexpr float kMaxExpansionDb = 0.05f;
    constexpr float kMaxRiseDb = 0.02f;

    bool passed = true;
    float worstGainDb = -120.0f;
    float worstRiseDb = 0.0f;
    juce::String table;
    juce::String fullScaleThd;

    for (int flavor = 0; flavor < static_cast<int>(TapeFlavor::NumFlavors); ++flavor)
    {
        const auto resolved = TapeFlavors::resolve(flavor, TapeFlavors::kDriveAnchor);
        float previousGainDb = 0.0f;
        bool havePrevious = false;
        juce::String row;
        for (const float level : levels)
        {
            const auto point = measureTransferPoint(resolved.drive, resolved.saturation, resolved.bias, level);
            worstGainDb = std::max(worstGainDb, point.gainDb);
            if (havePrevious)
                worstRiseDb = std::max(worstRiseDb, point.gainDb - previousGainDb);
            previousGainDb = point.gainDb;
            havePrevious = true;
            row << (row.isEmpty() ? "" : " ") << juce::String(level, 1) << "=" << juce::String(point.gainDb, 2);
            if (level == 0.0f)
                fullScaleThd << (fullScaleThd.isEmpty() ? "" : " ") << TapeFlavors::get(flavor).name << "="
                             << juce::String(point.thdPercent, 2) << "%";
        }
        table << (table.isEmpty() ? "" : "; ") << TapeFlavors::get(flavor).name << " " << row;
    }

    passed = worstGainDb <= kMaxExpansionDb && worstRiseDb <= kMaxRiseDb;

    BehaviorCheck check;
    check.name = "TapeRecordSaturatorMonotoneCompression";
    check.passed = passed;
    check.details = "maxGain=" + juce::String(worstGainDb, 3) + "dB(max " + juce::String(kMaxExpansionDb, 2) +
                    ") maxRise=" + juce::String(worstRiseDb, 3) + "dB(max " + juce::String(kMaxRiseDb, 2) +
                    ") fullScaleTHD[" + fullScaleThd + "] gainDbByInputDbfs[" + table + "]";
    return check;
}

}  // namespace

juce::String runTapeRecordSaturatorBehaviorSuite()
{
    const std::array<BehaviorCheck, 11> checks = {checkFiniteAndBounds(),    checkHistoryDependence(),
                                                  checkResetIdentity(),      checkSilenceDecay(),
                                                  checkStereoIndependence(), checkSmallSignalUnity(),
                                                  checkHotFluxCeiling(),     checkSampleRateConsistency(),
                                                  checkNonFiniteRecovery(),  checkSupportedSmoothingCallPatterns(),
                                                  checkMonotoneCompression()};

    juce::StringArray lines;
    int failures = 0;
    for (const auto& check : checks)
    {
        lines.add(TestUtilities::formatResult(check.passed, check.name, check.details));
        failures += check.passed ? 0 : 1;
    }

    lines.add("Summary: " + juce::String(static_cast<int>(checks.size() - static_cast<size_t>(failures))) + "/" +
              juce::String(static_cast<int>(checks.size())) + " passed");
    return lines.joinIntoString("\n");
}

}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
