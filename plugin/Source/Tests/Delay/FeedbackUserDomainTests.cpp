// FeedbackUserDomainTests: User-domain contracts for feedback travel, first-echo tone, presentation level, and output
//   headroom.

#include "FeedbackUserDomainTests.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../PluginProcessor.h"
#include "../Common/TestUtilities.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace SpatialEchoTests::Delay
{
namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;
constexpr int kWarmupSamples = 24000;
constexpr int kStimulusStart = 960;           // 20 ms of clean pre-roll in every capture.
constexpr int kClassicDelaySamples = 8400;    // Public Repeat Rate=0 -> 175 ms at 48 kHz.
constexpr int kLongSyncDelaySamples = 48000;  // 1/2 note at 120 BPM -> 1000 ms.
constexpr double kSignalFloorRms = 1.0e-7;
constexpr float kUnprotectedPeakLimit = 0.96f;
constexpr float kProtectedPeakLimit = 1.05f;

struct FixedPlayHead final : juce::AudioPlayHead
{
    double bpm = 120.0;
    double sampleRate = kSampleRate;
    int64_t samplePosition = 0;

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        juce::AudioPlayHead::PositionInfo info;
        info.setBpm(bpm);
        info.setIsPlaying(true);
        info.setTimeInSamples(samplePosition);
        const double seconds = static_cast<double>(samplePosition) / sampleRate;
        info.setTimeInSeconds(seconds);
        info.setPpqPosition(seconds * bpm / 60.0);
        return info;
    }

    void advance(int samples) noexcept { samplePosition += samples; }
};

struct RenderSettings
{
    float feedback = 0.0f;
    float mix = 0.5f;
    int tapeFlavor = 1;    // WARM
    int delaySetting = 0;  // H1 Echo
    bool pingPong = false;
    bool tempoSync = false;
    bool outputProtection = false;
};

struct RenderTrace
{
    std::vector<float> left;
    std::vector<float> right;
    bool parametersOk = true;
    bool finite = true;
    float peak = 0.0f;
    juce::String parameterFailures;
};

class ParameterWriter
{
public:
    explicit ParameterWriter(juce::AudioProcessorValueTreeState& stateToUse)
        : state(stateToUse)
    {
    }

    void setRequired(const char* id, float plainValue)
    {
        auto* parameter = state.getParameter(id);
        auto* raw = state.getRawParameterValue(id);
        if (parameter == nullptr || raw == nullptr)
        {
            failures.addIfNotAlreadyThere(juce::String(id) + " missing");
            return;
        }

        parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
        const float readback = raw->load();
        if (!std::isfinite(readback) || std::abs(readback - plainValue) > 1.0e-4f)
        {
            failures.addIfNotAlreadyThere(juce::String(id) + " readback=" + juce::String(readback, 5));
        }
    }

    bool ok() const noexcept { return failures.isEmpty(); }
    juce::String details() const { return failures.joinIntoString(","); }

private:
    juce::AudioProcessorValueTreeState& state;
    juce::StringArray failures;
};

bool configureProcessor(SpatialEchoAudioProcessor& processor, const RenderSettings& settings,
                        juce::String& failureDetails)
{
    ParameterWriter writer(processor.getValueTreeState());

    // Neutral input stage. Custom prevents the preset selector from rewriting
    // the three explicitly neutral controls on the first slow update.
    writer.setRequired("PreampPreset", 5.0f);
    writer.setRequired("InputLevel", 0.5f);
    writer.setRequired("Bass", 0.5f);
    writer.setRequired("Treble", 0.5f);

    writer.setRequired("DelaySetting", static_cast<float>(settings.delaySetting));
    writer.setRequired("RepeatRate", 0.0f);
    writer.setRequired("TempoSync", settings.tempoSync ? 1.0f : 0.0f);
    writer.setRequired("Head1NoteDivision", settings.tempoSync ? 1.0f : 2.0f);
    writer.setRequired("Head2NoteDivision", 3.0f);
    writer.setRequired("Head3NoteDivision", 4.0f);
    writer.setRequired("HeadRatioPreset", 0.0f);
    writer.setRequired("Intensity", settings.feedback);
    writer.setRequired("EchoSend", 1.0f);
    writer.setRequired("Power", 1.0f);
    writer.setRequired("AutoPanic", 0.0f);
    writer.setRequired("PingPong", settings.pingPong ? 1.0f : 0.0f);

    writer.setRequired("Mix", settings.mix);
    writer.setRequired("EchoLevel", 0.0f);
    // All-heads is exposed publicly only by an Echo+Reverb mode. Keep that
    // unavoidable product branch at its minimum and deterministic.
    writer.setRequired("ReverbLevel", -12.0f);
    writer.setRequired("ReverbMod", 0.0f);
    writer.setRequired("ReverbDuckAmount", 0.0f);
    writer.setRequired("ReverbRouting", 0.0f);
    writer.setRequired("OutputGain", 0.5f);
    writer.setRequired("MixLock", 0.0f);
    writer.setRequired("OutputProtection", settings.outputProtection ? 1.0f : 0.0f);
    writer.setRequired("FeedbackCompensation", 1.0f);

    writer.setRequired("TapeFlavor", static_cast<float>(settings.tapeFlavor));
    writer.setRequired("TapeDrive", 0.45f);
    writer.setRequired("SpatialEnabled", 0.0f);
    writer.setRequired("SpatialRouting", 0.0f);
    writer.setRequired("HeadPositionMode", 0.0f);

    failureDetails = writer.details();
    return writer.ok();
}

std::vector<float> makeBroadbandBurst(int length, float peakAmplitude, uint32_t seed)
{
    std::vector<float> burst(static_cast<size_t>(length), 0.0f);
    uint32_t state = seed;
    float peak = 0.0f;
    for (int i = 0; i < length; ++i)
    {
        state = state * 1664525u + 1013904223u;
        const float random = (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
        const float phase = static_cast<float>(i + 1) / static_cast<float>(length + 1);
        const float envelope = std::sin(juce::MathConstants<float>::pi * phase);
        const float value = random * envelope * envelope;
        burst[static_cast<size_t>(i)] = value;
        peak = std::max(peak, std::abs(value));
    }

    const float scale = peak > 0.0f ? peakAmplitude / peak : 0.0f;
    for (float& value : burst)
        value *= scale;
    return burst;
}

std::vector<float> makeToneBurst(float frequencyHz, int length, int fadeSamples, float peakAmplitude)
{
    std::vector<float> burst(static_cast<size_t>(length), 0.0f);
    for (int i = 0; i < length; ++i)
    {
        float envelope = 1.0f;
        if (i < fadeSamples)
            envelope = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * static_cast<float>(i) /
                                              static_cast<float>(std::max(1, fadeSamples)));
        else if (i >= length - fadeSamples)
        {
            const int remaining = length - 1 - i;
            envelope = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * static_cast<float>(remaining) /
                                              static_cast<float>(std::max(1, fadeSamples)));
        }

        const float phase =
            juce::MathConstants<float>::twoPi * frequencyHz * static_cast<float>(i) / static_cast<float>(kSampleRate);
        burst[static_cast<size_t>(i)] = peakAmplitude * envelope * std::sin(phase);
    }
    return burst;
}

template <typename SampleGenerator>
RenderTrace renderProcessor(SpatialEchoAudioProcessor& processor, const RenderSettings& settings, int totalSamples,
                            SampleGenerator&& generateSample)
{
    RenderTrace trace;
    trace.parametersOk = configureProcessor(processor, settings, trace.parameterFailures);
    if (!trace.parametersOk)
        return trace;

    FixedPlayHead playHead;
    processor.setPlayHead(settings.tempoSync ? &playHead : nullptr);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    juce::AudioBuffer<float> buffer(2, kBlockSize);
    juce::MidiBuffer midi;
    auto processAndInspect = [&](bool capture, int captureOffset)
    {
        processor.processBlock(buffer, midi);
        if (settings.tempoSync)
            playHead.advance(kBlockSize);

        for (int sample = 0; sample < kBlockSize; ++sample)
        {
            const int outputIndex = captureOffset + sample;
            if (capture && outputIndex >= totalSamples)
                break;

            const float left = buffer.getSample(0, sample);
            const float right = buffer.getSample(1, sample);
            const bool sampleFinite = std::isfinite(left) && std::isfinite(right);
            trace.finite = trace.finite && sampleFinite;
            if (!capture)
                continue;

            const float storedLeft = sampleFinite ? left : 0.0f;
            const float storedRight = sampleFinite ? right : 0.0f;
            trace.left.push_back(storedLeft);
            trace.right.push_back(storedRight);
            trace.peak = std::max(trace.peak, std::max(std::abs(storedLeft), std::abs(storedRight)));
        }
    };

    for (int offset = 0; offset < kWarmupSamples; offset += kBlockSize)
    {
        buffer.clear();
        processAndInspect(false, 0);
    }

    trace.left.reserve(static_cast<size_t>(totalSamples));
    trace.right.reserve(static_cast<size_t>(totalSamples));
    for (int offset = 0; offset < totalSamples; offset += kBlockSize)
    {
        buffer.clear();
        for (int sample = 0; sample < kBlockSize; ++sample)
        {
            const int timelineSample = offset + sample;
            if (timelineSample >= totalSamples)
                break;
            const auto input = generateSample(timelineSample);
            buffer.setSample(0, sample, input.first);
            buffer.setSample(1, sample, input.second);
        }
        processAndInspect(true, offset);
    }

    processor.setPlayHead(nullptr);
    return trace;
}

double stereoRms(const RenderTrace& trace, int startSample, int endSample)
{
    const int available = static_cast<int>(std::min(trace.left.size(), trace.right.size()));
    const int start = std::clamp(startSample, 0, available);
    const int end = std::clamp(endSample, start, available);
    double energy = 0.0;
    int count = 0;
    for (int i = start; i < end; ++i)
    {
        const double left = trace.left[static_cast<size_t>(i)];
        const double right = trace.right[static_cast<size_t>(i)];
        energy += left * left + right * right;
        count += 2;
    }
    return count > 0 ? std::sqrt(energy / static_cast<double>(count)) : 0.0;
}

double stereoFundamentalRms(const RenderTrace& trace, int startSample, int endSample, float frequencyHz)
{
    const int available = static_cast<int>(std::min(trace.left.size(), trace.right.size()));
    const int start = std::clamp(startSample, 0, available);
    const int end = std::clamp(endSample, start, available);
    const int count = end - start;
    if (count <= 0)
        return 0.0;

    double leftSin = 0.0;
    double leftCos = 0.0;
    double rightSin = 0.0;
    double rightCos = 0.0;
    for (int sample = start; sample < end; ++sample)
    {
        const double phase = juce::MathConstants<double>::twoPi * static_cast<double>(frequencyHz) *
                             static_cast<double>(sample) / kSampleRate;
        const double sine = std::sin(phase);
        const double cosine = std::cos(phase);
        leftSin += static_cast<double>(trace.left[static_cast<size_t>(sample)]) * sine;
        leftCos += static_cast<double>(trace.left[static_cast<size_t>(sample)]) * cosine;
        rightSin += static_cast<double>(trace.right[static_cast<size_t>(sample)]) * sine;
        rightCos += static_cast<double>(trace.right[static_cast<size_t>(sample)]) * cosine;
    }

    const double projectionScale = 2.0 / static_cast<double>(count);
    const double leftAmplitude = projectionScale * std::hypot(leftSin, leftCos);
    const double rightAmplitude = projectionScale * std::hypot(rightSin, rightCos);
    return std::sqrt(0.25 * (leftAmplitude * leftAmplitude + rightAmplitude * rightAmplitude));
}

double dbRatio(double numerator, double denominator)
{
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || numerator <= kSignalFloorRms ||
        denominator <= kSignalFloorRms)
        return std::numeric_limits<double>::quiet_NaN();
    return 20.0 * std::log10(numerator / denominator);
}

double median(std::array<double, 4> values)
{
    std::sort(values.begin(), values.end());
    return 0.5 * (values[1] + values[2]);
}

juce::String dbString(double value, int decimals = 2)
{
    return std::isfinite(value) ? juce::String(value, decimals) + "dB" : juce::String("n/a");
}

double levelDb(double linear)
{
    return std::isfinite(linear) && linear > 0.0
               ? static_cast<double>(TestUtilities::linearToDb(static_cast<float>(linear)))
               : -100.0;
}

void appendFailureDetails(juce::StringArray& failures, const juce::String& details)
{
    if (details.isNotEmpty())
        failures.addIfNotAlreadyThere(details);
}

struct ToneMeasurement
{
    double dryRms = 0.0;
    double repeatRms = 0.0;
    double wetToDryDb = std::numeric_limits<double>::quiet_NaN();
    float peak = 0.0f;
    bool valid = false;
    juce::String parameterFailures;
};

ToneMeasurement measureRepeatTone(SpatialEchoAudioProcessor& processor, float frequencyHz, bool longSync,
                                  int repeatIndex = 1, float feedback = 0.0f)
{
    constexpr int burstSamples = 2880;  // 60 ms; 30 cycles at the 500 Hz anchor.
    constexpr int fadeSamples = 240;
    constexpr int marginSamples = 720;
    const auto burst = makeToneBurst(frequencyHz, burstSamples, fadeSamples, 0.35f);
    const int delaySamples = longSync ? kLongSyncDelaySamples : kClassicDelaySamples;
    repeatIndex = std::max(1, repeatIndex);
    const int repeatOffset = repeatIndex * delaySamples;
    const int totalSamples = kStimulusStart + repeatOffset + burstSamples + marginSamples + 512;

    RenderSettings settings;
    settings.feedback = feedback;
    settings.mix = 0.5f;
    settings.tapeFlavor = 1;  // WARM
    settings.tempoSync = longSync;

    const auto trace = renderProcessor(processor, settings, totalSamples,
                                       [&](int sample) -> std::pair<float, float>
                                       {
                                           const int burstIndex = sample - kStimulusStart;
                                           const float value = (burstIndex >= 0 && burstIndex < burstSamples)
                                                                   ? burst[static_cast<size_t>(burstIndex)]
                                                                   : 0.0f;
                                           return {value, value};
                                       });

    ToneMeasurement result;
    result.parameterFailures = trace.parameterFailures;
    result.peak = trace.peak;
    result.dryRms = stereoFundamentalRms(trace, kStimulusStart + fadeSamples,
                                         kStimulusStart + burstSamples - fadeSamples, frequencyHz);
    result.repeatRms = stereoFundamentalRms(trace, kStimulusStart + repeatOffset + fadeSamples,
                                            kStimulusStart + repeatOffset + burstSamples - fadeSamples, frequencyHz);
    result.wetToDryDb = dbRatio(result.repeatRms, result.dryRms);
    result.valid = trace.parametersOk && trace.finite && std::isfinite(result.wetToDryDb) &&
                   result.dryRms > kSignalFloorRms && result.repeatRms > kSignalFloorRms &&
                   trace.peak < kUnprotectedPeakLimit;
    return result;
}

enum class ProgramSource
{
    BroadbandTransient,
    SustainedBass,
    BrightTransients
};

enum class StereoImage
{
    Mono,
    Correlated,
    OppositePolarity
};

struct HeadroomCase
{
    const char* label;
    ProgramSource source;
    StereoImage image;
    int delaySetting;
    bool pingPong;
    float feedback;
    bool protection;
};

std::pair<float, float> applyStereoImage(float base, StereoImage image, int sample)
{
    switch (image)
    {
        case StereoImage::Mono:
            return {base, base};
        case StereoImage::Correlated:
        {
            const float widthComponent =
                0.08f * base * std::sin(juce::MathConstants<float>::twoPi * static_cast<float>(sample) / 173.0f);
            return {base, 0.82f * base + widthComponent};
        }
        case StereoImage::OppositePolarity:
            return {base, -base};
    }
    return {0.0f, 0.0f};
}

}  // namespace

juce::String runUserFeedbackDecayProfileTest(SpatialEchoAudioProcessor&)
{
    // Dispatchers pass their live processor by convention. Use a dedicated
    // production processor so this measurement cannot rewrite host state.
    SpatialEchoAudioProcessor processor;

    struct Contract
    {
        float raw;
        double minimumDb;
        double maximumDb;
    };

    // The design calls the first three targets approximate. These bands make
    // that language executable without allowing an adjacent knob zone to pass.
    constexpr std::array<Contract, 5> contracts{{{0.25f, -10.50, -7.50},
                                                 {0.50f, -5.75, -3.25},
                                                 {0.75f, -3.00, -1.00},
                                                 {0.90f, -1.00, 0.50},
                                                 {1.00f, -0.25, 0.50}}};

    constexpr int burstSamples = 384;
    constexpr int repeatWindowMargin = 960;
    constexpr int repeatsToCapture = 12;
    const auto burst = makeBroadbandBurst(burstSamples, 0.30f, 0xF33DBA11u);
    const int totalSamples =
        kStimulusStart + repeatsToCapture * kClassicDelaySamples + repeatWindowMargin + burstSamples;

    bool passed = true;
    juce::StringArray failures;
    juce::String rows;
    double previousMedian = -std::numeric_limits<double>::infinity();
    double normalReferenceMedian = std::numeric_limits<double>::quiet_NaN();
    bool noiseDomainContractOk = true;

    struct MapPoint
    {
        float raw;
        float expectedGain;
    };
    constexpr std::array<MapPoint, 6> mapAnchors{
        {{0.00f, 0.000f}, {0.25f, 0.374f}, {0.50f, 0.629f}, {0.75f, 0.838f}, {0.90f, 1.025f}, {1.00f, 1.040f}}};
    constexpr float mapTolerance = 1.0e-6f;
    bool mapContractOk = true;
    for (size_t index = 0; index < mapAnchors.size(); ++index)
    {
        const auto& anchor = mapAnchors[index];
        const float mappedAnchor = SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(anchor.raw);
        mapContractOk = mapContractOk && std::isfinite(mappedAnchor) &&
                        std::abs(mappedAnchor - anchor.expectedGain) <= mapTolerance;

        if (index == 0)
            continue;

        const auto& previous = mapAnchors[index - 1];
        const float midpoint = 0.5f * (previous.raw + anchor.raw);
        const float expectedMidpoint = 0.5f * (previous.expectedGain + anchor.expectedGain);
        const float mappedMidpoint = SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(midpoint);
        const float immediatelyBelow = SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(anchor.raw - 1.0e-5f);
        const float immediatelyAbove = SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(anchor.raw + 1.0e-5f);
        mapContractOk = mapContractOk && std::isfinite(mappedMidpoint) &&
                        std::abs(mappedMidpoint - expectedMidpoint) <= mapTolerance &&
                        immediatelyBelow <= mappedAnchor && mappedAnchor <= immediatelyAbove &&
                        std::abs(immediatelyAbove - immediatelyBelow) <= 5.0e-5f;
    }

    const float mappedBelowRange = SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(-1.0f);
    const float mappedAboveRange = SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(2.0f);
    const float mappedNan =
        SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(std::numeric_limits<float>::quiet_NaN());
    const float mappedPositiveInf =
        SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(std::numeric_limits<float>::infinity());
    const float mappedNegativeInf =
        SpatialEchoAudioProcessor::debugMapFeedbackToLoopGain(-std::numeric_limits<float>::infinity());
    mapContractOk = mapContractOk && juce::exactlyEqual(mappedBelowRange, mapAnchors.front().expectedGain) &&
                    juce::exactlyEqual(mappedAboveRange, mapAnchors.back().expectedGain) &&
                    juce::exactlyEqual(mappedNan, mapAnchors.front().expectedGain) &&
                    juce::exactlyEqual(mappedPositiveInf, mapAnchors.front().expectedGain) &&
                    juce::exactlyEqual(mappedNegativeInf, mapAnchors.front().expectedGain);
    passed = passed && mapContractOk;
    if (!mapContractOk)
        failures.add("mapping-contract");

    for (const auto& contract : contracts)
    {
        RenderSettings settings;
        settings.feedback = contract.raw;
        settings.mix = 1.0f;
        settings.tapeFlavor = 0;    // CLEAN
        settings.delaySetting = 0;  // isolated H1, no reverb
        settings.pingPong = false;
        settings.tempoSync = false;
        settings.outputProtection = false;

        const auto trace = renderProcessor(processor, settings, totalSamples,
                                           [&](int sample) -> std::pair<float, float>
                                           {
                                               const int burstIndex = sample - kStimulusStart;
                                               const float value = (burstIndex >= 0 && burstIndex < burstSamples)
                                                                       ? burst[static_cast<size_t>(burstIndex)]
                                                                       : 0.0f;
                                               return {value, value};
                                           });

        std::array<double, 5> repeatRms{};
        for (int repeat = 1; repeat <= 5; ++repeat)
        {
            const int start = kStimulusStart + repeat * kClassicDelaySamples - repeatWindowMargin;
            const int end = kStimulusStart + repeat * kClassicDelaySamples + burstSamples + repeatWindowMargin;
            repeatRms[static_cast<size_t>(repeat - 1)] = stereoRms(trace, start, end);
        }

        std::array<double, 4> deltas{{dbRatio(repeatRms[1], repeatRms[0]), dbRatio(repeatRms[2], repeatRms[1]),
                                      dbRatio(repeatRms[3], repeatRms[2]), dbRatio(repeatRms[4], repeatRms[3])}};
        const bool deltasFinite =
            std::all_of(deltas.begin(), deltas.end(), [](double value) { return std::isfinite(value); });
        const double medianDb = deltasFinite ? median(deltas) : std::numeric_limits<double>::quiet_NaN();
        const bool targetOk =
            std::isfinite(medianDb) && medianDb >= contract.minimumDb && medianDb <= contract.maximumDb;
        const bool monotonicOk =
            !std::isfinite(previousMedian) || !std::isfinite(medianDb) || medianDb + 0.10 >= previousMedian;
        double lateMedianDb = std::numeric_limits<double>::quiet_NaN();
        double lateTotalGrowthDb = std::numeric_limits<double>::quiet_NaN();
        bool lateBloomBounded = true;
        if (contract.raw == 1.0f)
        {
            std::array<double, 5> lateRms{};
            for (int repeat = 8; repeat <= 12; ++repeat)
            {
                const int start = kStimulusStart + repeat * kClassicDelaySamples - repeatWindowMargin;
                const int end = kStimulusStart + repeat * kClassicDelaySamples + burstSamples + repeatWindowMargin;
                lateRms[static_cast<size_t>(repeat - 8)] = stereoRms(trace, start, end);
            }

            std::array<double, 4> lateDeltas{{dbRatio(lateRms[1], lateRms[0]), dbRatio(lateRms[2], lateRms[1]),
                                              dbRatio(lateRms[3], lateRms[2]), dbRatio(lateRms[4], lateRms[3])}};
            const bool lateFinite =
                std::all_of(lateDeltas.begin(), lateDeltas.end(), [](double value) { return std::isfinite(value); });
            const bool lateSignal = std::all_of(lateRms.begin(), lateRms.end(), [](double value)
                                                { return std::isfinite(value) && value > kSignalFloorRms; });
            lateMedianDb = lateFinite ? median(lateDeltas) : std::numeric_limits<double>::quiet_NaN();
            lateTotalGrowthDb = dbRatio(lateRms[4], lateRms[0]);
            // Early repeats may bloom, but by repeats 8-12 the loop must have
            // reached a bounded plateau instead of retaining even a slow
            // exponential gain that would only run away after this capture.
            lateBloomBounded = lateFinite && lateSignal && std::isfinite(lateTotalGrowthDb) && lateMedianDb <= 0.10 &&
                               lateTotalGrowthDb <= 0.50;
        }

        const bool bounded = trace.peak < kUnprotectedPeakLimit && lateBloomBounded;
        const float noiseFeedbackIntensity = processor.debugGetNoiseFeedbackIntensity();
        const bool noiseDomainOk =
            std::isfinite(noiseFeedbackIntensity) && std::abs(noiseFeedbackIntensity - contract.raw) <= 1.0e-6f;
        noiseDomainContractOk = noiseDomainContractOk && noiseDomainOk;
        const bool signalOk = std::all_of(repeatRms.begin(), repeatRms.end(),
                                          [](double value) { return std::isfinite(value) && value > kSignalFloorRms; });
        const bool rowPassed = trace.parametersOk && trace.finite && signalOk && deltasFinite && targetOk &&
                               monotonicOk && bounded && noiseDomainOk;
        passed = passed && rowPassed;
        if (!rowPassed)
            failures.add("raw=" + juce::String(contract.raw, 2));
        if (!noiseDomainOk)
            failures.add("noise-domain-raw=" + juce::String(contract.raw, 2));
        appendFailureDetails(failures, trace.parameterFailures);

        rows << (rows.isEmpty() ? "" : "; ") << "raw=" << juce::String(contract.raw, 2)
             << " d2..5=" << dbString(deltas[0]) << "/" << dbString(deltas[1]) << "/" << dbString(deltas[2]) << "/"
             << dbString(deltas[3]) << " median=" << dbString(medianDb, 3) << " target=["
             << juce::String(contract.minimumDb, 2) << "," << juce::String(contract.maximumDb, 2) << "]dB"
             << " peak=" << juce::String(trace.peak, 3);
        if (contract.raw == 1.0f)
            rows << " lateMedian8..12=" << dbString(lateMedianDb) << " max=0.10dB"
                 << " lateGrowth8->12=" << dbString(lateTotalGrowthDb) << " max=0.50dB";
        if (contract.raw == 0.75f)
            normalReferenceMedian = medianDb;
        previousMedian = medianDb;
    }

    // Long-horizon self-oscillation trajectory at the top of the knob.
    // debugMapFeedbackToLoopGain(1.0) == 1.040, so the loop is deliberately
    // above unity here and the tail is MEANT to bloom. The physical safety
    // property is not "stops growing by an arbitrary repeat index" but
    // "loop gain falls as level rises", i.e. growth decelerates and the tail
    // converges. An accelerating tail means the record stage is adding gain
    // with level, which no magnetic recording process does.
    constexpr int kTrajectoryRepeats = 120;
    const int trajectorySamples =
        kStimulusStart + kTrajectoryRepeats * kClassicDelaySamples + repeatWindowMargin + burstSamples;
    RenderSettings trajectorySettings;
    trajectorySettings.feedback = 1.0f;
    trajectorySettings.mix = 1.0f;
    trajectorySettings.tapeFlavor = 0;  // CLEAN
    trajectorySettings.delaySetting = 0;
    trajectorySettings.pingPong = false;
    trajectorySettings.tempoSync = false;
    trajectorySettings.outputProtection = false;
    const auto trajectoryTrace = renderProcessor(processor, trajectorySettings, trajectorySamples,
                                                 [&](int sample) -> std::pair<float, float>
                                                 {
                                                     const int burstIndex = sample - kStimulusStart;
                                                     const float value = (burstIndex >= 0 && burstIndex < burstSamples)
                                                                             ? burst[static_cast<size_t>(burstIndex)]
                                                                             : 0.0f;
                                                     return {value, value};
                                                 });

    std::array<double, kTrajectoryRepeats + 1> trajectoryRms{};
    for (int repeat = 1; repeat <= kTrajectoryRepeats; ++repeat)
    {
        const int start = kStimulusStart + repeat * kClassicDelaySamples - repeatWindowMargin;
        const int end = kStimulusStart + repeat * kClassicDelaySamples + burstSamples + repeatWindowMargin;
        trajectoryRms[static_cast<size_t>(repeat)] = stereoRms(trajectoryTrace, start, end);
    }
    auto windowGrowthDb = [&](int first, int last) -> double
    {
        if (first < 1 || last > kTrajectoryRepeats || last <= first)
            return std::numeric_limits<double>::quiet_NaN();
        return dbRatio(trajectoryRms[static_cast<size_t>(last)], trajectoryRms[static_cast<size_t>(first)]) /
               static_cast<double>(last - first);
    };
    const double growthEarlyDb = windowGrowthDb(2, 8);
    const double growthRampDb = windowGrowthDb(8, 20);
    const double growthBloomDb = windowGrowthDb(20, 40);
    const double growthPeakDb = windowGrowthDb(40, 70);
    const double growthSettleDb = windowGrowthDb(70, 100);
    const double growthFinalDb = windowGrowthDb(100, 120);
    const double totalBloomDb = dbRatio(trajectoryRms[kTrajectoryRepeats], trajectoryRms[1]);
    // The early windows are dominated by the broadband burst losing its top
    // end to the feedback low-pass, so they are reported but not gated. What
    // is gated is the LATE phase: once the spectrum has settled, the growth
    // rate must be falling. That is what "loop gain drops as level rises"
    // looks like from outside; an expansive record stage does the opposite.
    constexpr double kDecelerationToleranceDb = 0.02;
    // 21 s of deliberate self-oscillation. A loop with an expansive record
    // stage passes this figure inside 7 s (+19.6 dB by repeat 40) and drives
    // the output past full scale.
    constexpr double kMaximumBloomDb = 10.0;
    const bool trajectoryFinite = std::isfinite(growthEarlyDb) && std::isfinite(growthRampDb) &&
                                  std::isfinite(growthBloomDb) && std::isfinite(growthPeakDb) &&
                                  std::isfinite(growthSettleDb) && std::isfinite(growthFinalDb) &&
                                  std::isfinite(totalBloomDb);
    const bool converges = trajectoryFinite && growthSettleDb <= growthPeakDb + kDecelerationToleranceDb &&
                           growthFinalDb <= growthSettleDb + kDecelerationToleranceDb;
    const bool trajectoryBounded = trajectoryTrace.parametersOk && trajectoryTrace.finite &&
                                   trajectoryTrace.peak < kUnprotectedPeakLimit && trajectoryFinite &&
                                   totalBloomDb <= kMaximumBloomDb;
    const bool trajectoryOk = converges && trajectoryBounded;
    passed = passed && trajectoryOk;
    if (!trajectoryOk)
        failures.add("selfosc-trajectory");
    appendFailureDetails(failures, trajectoryTrace.parameterFailures);

    RenderSettings pingPongSettings;
    pingPongSettings.feedback = 0.75f;
    pingPongSettings.mix = 1.0f;
    pingPongSettings.tapeFlavor = 0;    // CLEAN
    pingPongSettings.delaySetting = 0;  // isolated H1
    pingPongSettings.pingPong = true;
    pingPongSettings.outputProtection = false;
    const auto pingPongTrace = renderProcessor(processor, pingPongSettings, totalSamples,
                                               [&](int sample) -> std::pair<float, float>
                                               {
                                                   const int burstIndex = sample - kStimulusStart;
                                                   const float value = (burstIndex >= 0 && burstIndex < burstSamples)
                                                                           ? burst[static_cast<size_t>(burstIndex)]
                                                                           : 0.0f;
                                                   return {value, value};
                                               });
    std::array<double, 5> pingPongRepeatRms{};
    for (int repeat = 1; repeat <= 5; ++repeat)
    {
        const int start = kStimulusStart + repeat * kClassicDelaySamples - repeatWindowMargin;
        const int end = kStimulusStart + repeat * kClassicDelaySamples + burstSamples + repeatWindowMargin;
        pingPongRepeatRms[static_cast<size_t>(repeat - 1)] = stereoRms(pingPongTrace, start, end);
    }
    std::array<double, 4> pingPongDeltas{
        {dbRatio(pingPongRepeatRms[1], pingPongRepeatRms[0]), dbRatio(pingPongRepeatRms[2], pingPongRepeatRms[1]),
         dbRatio(pingPongRepeatRms[3], pingPongRepeatRms[2]), dbRatio(pingPongRepeatRms[4], pingPongRepeatRms[3])}};
    const bool pingPongFinite =
        std::all_of(pingPongDeltas.begin(), pingPongDeltas.end(), [](double value) { return std::isfinite(value); });
    const double pingPongMedian = pingPongFinite ? median(pingPongDeltas) : std::numeric_limits<double>::quiet_NaN();
    const double stereoModeDecayDifference = std::abs(pingPongMedian - normalReferenceMedian);
    const bool stereoModeParityOk = pingPongTrace.parametersOk && pingPongTrace.finite &&
                                    std::isfinite(stereoModeDecayDifference) && stereoModeDecayDifference <= 0.50;
    passed = passed && stereoModeParityOk;
    if (!stereoModeParityOk)
        failures.add("normal-pingpong-decay-parity");
    appendFailureDetails(failures, pingPongTrace.parameterFailures);

    juce::String result;
    result << "UserFeedbackDecayProfile: map=" << (mapContractOk ? "ok" : "invalid")
           << " noiseDomain=" << (noiseDomainContractOk ? "ok" : "invalid") << "; clean/H1/Normal/175ms; " << rows
           << "; selfOscTrajectory perRepeat r2->8=" << dbString(growthEarlyDb) << "/r8->20=" << dbString(growthRampDb)
           << "/r20->40=" << dbString(growthBloomDb) << "/r40->70=" << dbString(growthPeakDb)
           << "/r70->100=" << dbString(growthSettleDb) << "/r100->120=" << dbString(growthFinalDb)
           << " totalBloom=" << dbString(totalBloomDb) << " max=" << juce::String(kMaximumBloomDb, 2) << "dB"
           << " peak=" << juce::String(trajectoryTrace.peak, 3)
           << "; Normal/PingPong@.75=" << dbString(normalReferenceMedian) << "/" << dbString(pingPongMedian)
           << " diff=" << dbString(stereoModeDecayDifference) << " max=0.50dB";
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

static juce::String runFirstRepeatMeasurement(bool requireLevel, const char* testLabel)
{
    SpatialEchoAudioProcessor processor;

    constexpr int burstSamples = 576;
    constexpr int windowMargin = 720;
    const auto transient = makeBroadbandBurst(burstSamples, 0.50f, 0x1A57E001u);
    const int transientSamples = kStimulusStart + kClassicDelaySamples + burstSamples + windowMargin + 512;

    RenderSettings transientSettings;
    transientSettings.feedback = 0.0f;
    transientSettings.mix = 0.5f;
    transientSettings.tapeFlavor = 1;  // WARM
    const auto transientTrace = renderProcessor(processor, transientSettings, transientSamples,
                                                [&](int sample) -> std::pair<float, float>
                                                {
                                                    const int burstIndex = sample - kStimulusStart;
                                                    const float value = (burstIndex >= 0 && burstIndex < burstSamples)
                                                                            ? transient[static_cast<size_t>(burstIndex)]
                                                                            : 0.0f;
                                                    return {value, value};
                                                });

    const double dryRms =
        stereoRms(transientTrace, kStimulusStart - windowMargin, kStimulusStart + burstSamples + windowMargin);
    const double firstRepeatRms = stereoRms(transientTrace, kStimulusStart + kClassicDelaySamples - windowMargin,
                                            kStimulusStart + kClassicDelaySamples + burstSamples + windowMargin);
    const double firstRepeatToDryDb = dbRatio(firstRepeatRms, dryRms);

    const ToneMeasurement warm500 = measureRepeatTone(processor, 500.0f, false);
    const ToneMeasurement warm2k = measureRepeatTone(processor, 2000.0f, false);
    const ToneMeasurement warm5k = measureRepeatTone(processor, 5000.0f, false);
    const ToneMeasurement sync500 = measureRepeatTone(processor, 500.0f, true);
    const ToneMeasurement sync5k = measureRepeatTone(processor, 5000.0f, true);
    const ToneMeasurement repeat1_500 = measureRepeatTone(processor, 500.0f, false, 1, 0.75f);
    const ToneMeasurement repeat1_5k = measureRepeatTone(processor, 5000.0f, false, 1, 0.75f);
    const ToneMeasurement repeat2_500 = measureRepeatTone(processor, 500.0f, false, 2, 0.75f);
    const ToneMeasurement repeat2_5k = measureRepeatTone(processor, 5000.0f, false, 2, 0.75f);
    const ToneMeasurement repeat3_500 = measureRepeatTone(processor, 500.0f, false, 3, 0.75f);
    const ToneMeasurement repeat3_5k = measureRepeatTone(processor, 5000.0f, false, 3, 0.75f);

    const double warm2kRelativeDb = warm2k.wetToDryDb - warm500.wetToDryDb;
    const double warm5kRelativeDb = warm5k.wetToDryDb - warm500.wetToDryDb;
    const double sync5kRelativeDb = sync5k.wetToDryDb - sync500.wetToDryDb;
    const double repeat1_5kRelativeDb = repeat1_5k.wetToDryDb - repeat1_500.wetToDryDb;
    const double repeat2_5kRelativeDb = repeat2_5k.wetToDryDb - repeat2_500.wetToDryDb;
    const double repeat3_5kRelativeDb = repeat3_5k.wetToDryDb - repeat3_500.wetToDryDb;
    const double repeat2DarkeningDb = repeat2_5kRelativeDb - repeat1_5kRelativeDb;
    const double repeat3DarkeningDb = repeat3_5kRelativeDb - repeat2_5kRelativeDb;

    const bool levelOk = std::isfinite(firstRepeatToDryDb) && firstRepeatToDryDb >= -2.0 && firstRepeatToDryDb <= 1.0;
    const bool progressiveDarkeningOk = std::isfinite(repeat2DarkeningDb) && std::isfinite(repeat3DarkeningDb) &&
                                        repeat2DarkeningDb >= -5.0 && repeat2DarkeningDb <= -2.0 &&
                                        repeat3DarkeningDb >= -5.0 && repeat3DarkeningDb <= -2.0;
    const bool spectrumOk = std::isfinite(warm2kRelativeDb) && std::isfinite(warm5kRelativeDb) &&
                            std::isfinite(sync5kRelativeDb) && warm2kRelativeDb >= -2.5 && warm5kRelativeDb >= -8.0 &&
                            sync5kRelativeDb >= -12.0 && progressiveDarkeningOk;
    const bool transientValid = transientTrace.parametersOk && transientTrace.finite && dryRms > kSignalFloorRms &&
                                firstRepeatRms > kSignalFloorRms && transientTrace.peak < kUnprotectedPeakLimit;
    const bool toneRowsValid = warm500.valid && warm2k.valid && warm5k.valid && sync500.valid && sync5k.valid &&
                               repeat1_500.valid && repeat1_5k.valid && repeat2_500.valid && repeat2_5k.valid &&
                               repeat3_500.valid && repeat3_5k.valid;
    const bool passed = transientValid && toneRowsValid && spectrumOk && (!requireLevel || levelOk);

    juce::StringArray failures;
    appendFailureDetails(failures, transientTrace.parameterFailures);
    appendFailureDetails(failures, warm500.parameterFailures);
    appendFailureDetails(failures, warm2k.parameterFailures);
    appendFailureDetails(failures, warm5k.parameterFailures);
    appendFailureDetails(failures, sync500.parameterFailures);
    appendFailureDetails(failures, sync5k.parameterFailures);
    appendFailureDetails(failures, repeat1_500.parameterFailures);
    appendFailureDetails(failures, repeat1_5k.parameterFailures);
    appendFailureDetails(failures, repeat2_500.parameterFailures);
    appendFailureDetails(failures, repeat2_5k.parameterFailures);
    appendFailureDetails(failures, repeat3_500.parameterFailures);
    appendFailureDetails(failures, repeat3_5k.parameterFailures);

    juce::String result;
    result << testLabel << ": Mix=.50 EchoLevel=0dB Warm; broadband=" << dbString(firstRepeatToDryDb)
           << " target=[-2,+1]dB"
           << " valid=" << (transientValid ? "yes" : "no")
           << "; 175ms wet/dry 500/2k/5k=" << dbString(warm500.wetToDryDb) << "/" << dbString(warm2k.wetToDryDb) << "/"
           << dbString(warm5k.wetToDryDb) << " rel2k=" << dbString(warm2kRelativeDb) << " min=-2.50dB"
           << " rel5k=" << dbString(warm5kRelativeDb) << " min=-8.00dB"
           << "; sync=1/2@120BPM(1000ms) rel5k/500=" << dbString(sync5kRelativeDb) << " min=-12.00dB"
           << "; feedback=.75 repeat1/2/3 rel5k/500=" << dbString(repeat1_5kRelativeDb) << "/"
           << dbString(repeat2_5kRelativeDb) << "/" << dbString(repeat3_5kRelativeDb)
           << " darkening=" << dbString(repeat2DarkeningDb) << "/" << dbString(repeat3DarkeningDb)
           << " target=[-5,-2]dB/repeat"
           << " toneRowsValid=" << (toneRowsValid ? "yes" : "no");
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

juce::String runFirstRepeatToneDarkeningTest(SpatialEchoAudioProcessor&)
{
    return runFirstRepeatMeasurement(false, "FirstRepeatToneDarkening");
}

juce::String runFirstRepeatLevelSpectrumTest(SpatialEchoAudioProcessor&)
{
    return runFirstRepeatMeasurement(true, "FirstRepeatLevelSpectrum");
}

juce::String runEchoPresentationHeadroomTest(SpatialEchoAudioProcessor&)
{
    SpatialEchoAudioProcessor processor;

    // Pairwise coverage, not a Cartesian explosion: every required source,
    // head-count, stereo mode, feedback anchor, and protection state appears.
    constexpr std::array<HeadroomCase, 7> cases{
        {{"mono-transient/H1/Normal/F0/off", ProgramSource::BroadbandTransient, StereoImage::Mono, 0, false, 0.00f,
          false},
         {"correlated/H1/PingPong/F.75/off", ProgramSource::BroadbandTransient, StereoImage::Correlated, 0, true, 0.75f,
          false},
         {"opposite/3H/Normal/F.90/off", ProgramSource::BroadbandTransient, StereoImage::OppositePolarity, 10, false,
          0.90f, false},
         {"bass/H1/Normal/F1/off", ProgramSource::SustainedBass, StereoImage::Mono, 0, false, 1.00f, false},
         {"bass/H1/Normal/F1/on", ProgramSource::SustainedBass, StereoImage::Mono, 0, false, 1.00f, true},
         {"bright/3H/PingPong/F1/off", ProgramSource::BrightTransients, StereoImage::Correlated, 10, true, 1.00f,
          false},
         {"bright/3H/PingPong/F1/on", ProgramSource::BrightTransients, StereoImage::Correlated, 10, true, 1.00f,
          true}}};

    constexpr int totalSamples = 96000;  // Two seconds: >11 H1 round trips.
    constexpr int transientSamples = 576;
    constexpr int brightBurstSamples = 384;
    constexpr int firstWindowMargin = 720;
    const auto broadband = makeBroadbandBurst(transientSamples, 0.55f, 0xE0A0B055u);
    const auto bright = makeBroadbandBurst(brightBurstSamples, 0.60f, 0xB817E001u);

    bool passed = true;
    bool referenceLevelMeasured = false;
    double referenceLevelDb = std::numeric_limits<double>::quiet_NaN();
    juce::StringArray failures;
    juce::String rows;

    for (const auto& testCase : cases)
    {
        RenderSettings settings;
        settings.feedback = testCase.feedback;
        settings.mix = 0.5f;
        settings.tapeFlavor = 1;  // WARM
        settings.delaySetting = testCase.delaySetting;
        settings.pingPong = testCase.pingPong;
        settings.outputProtection = testCase.protection;

        const auto trace = renderProcessor(
            processor, settings, totalSamples,
            [&](int sample) -> std::pair<float, float>
            {
                float base = 0.0f;
                if (testCase.source == ProgramSource::BroadbandTransient)
                {
                    const int burstIndex = sample - kStimulusStart;
                    if (burstIndex >= 0 && burstIndex < transientSamples)
                        base = broadband[static_cast<size_t>(burstIndex)];
                }
                else if (testCase.source == ProgramSource::SustainedBass)
                {
                    constexpr int sustainSamples = 57600;
                    const int position = sample - kStimulusStart;
                    if (position >= 0 && position < sustainSamples)
                    {
                        const int edge = std::min(position, sustainSamples - 1 - position);
                        const float envelope = juce::jlimit(0.0f, 1.0f, static_cast<float>(edge) / 480.0f);
                        base = 0.35f * envelope *
                               std::sin(juce::MathConstants<float>::twoPi * 90.0f * static_cast<float>(position) /
                                        static_cast<float>(kSampleRate));
                    }
                }
                else
                {
                    constexpr std::array<int, 4> burstStarts{{960, 20160, 39360, 58560}};
                    for (const int start : burstStarts)
                    {
                        const int burstIndex = sample - start;
                        if (burstIndex >= 0 && burstIndex < brightBurstSamples)
                        {
                            // First difference rejects low-frequency energy and
                            // turns the deterministic burst into a bright transient.
                            const float current = bright[static_cast<size_t>(burstIndex)];
                            const float previous = burstIndex > 0 ? bright[static_cast<size_t>(burstIndex - 1)] : 0.0f;
                            base += 0.70f * (current - previous);
                        }
                    }
                    base = juce::jlimit(-0.70f, 0.70f, base);
                }
                return applyStereoImage(base, testCase.image, sample);
            });

        const double wholeRms = stereoRms(trace, 0, totalSamples);
        const float peakLimit = testCase.protection ? kProtectedPeakLimit : kUnprotectedPeakLimit;
        const bool rowPassed =
            trace.parametersOk && trace.finite && wholeRms > kSignalFloorRms && trace.peak <= peakLimit;
        passed = passed && rowPassed;
        if (!rowPassed)
            failures.add(testCase.label);
        appendFailureDetails(failures, trace.parameterFailures);

        if (!referenceLevelMeasured)
        {
            const double dryRms = stereoRms(trace, kStimulusStart - firstWindowMargin,
                                            kStimulusStart + transientSamples + firstWindowMargin);
            const double repeatRms =
                stereoRms(trace, kStimulusStart + kClassicDelaySamples - firstWindowMargin,
                          kStimulusStart + kClassicDelaySamples + transientSamples + firstWindowMargin);
            referenceLevelDb = dbRatio(repeatRms, dryRms);
            referenceLevelMeasured = true;
        }

        rows << (rows.isEmpty() ? "" : "; ") << testCase.label << " peak=" << juce::String(trace.peak, 3)
             << " limit=" << juce::String(peakLimit, 2) << " rms=" << dbString(levelDb(wholeRms))
             << " finite=" << (trace.finite ? "yes" : "no")
             << " signal=" << (wholeRms > kSignalFloorRms ? "yes" : "no");
    }

    // This headroom matrix deliberately uses a hotter, differently seeded burst
    // (peak 0.55) than FirstRepeatLevelSpectrum, and it is the peak/headroom
    // fixture, not the level fixture. The canonical [-2,+1] dB calibration gate
    // stays on FirstRepeatLevelSpectrum; here the floor is extended by one dB.
    //
    // Reason, measured: the record stage is monotonically compressive, so a
    // hotter stimulus must render a QUIETER first repeat than the canonical
    // one. It cannot do otherwise. An expansive record curve adds +1.47 dB at
    // -6 dBFS, which would let a hot burst read the same as a quiet one.
    // Isolated 2x2 measurement of this fixture:
    //   expansive curve + 149 Hz playback high-pass  -2.03 dB (0.02 dB margin)
    //   expansive curve + 40 Hz  playback high-pass  -1.87 dB
    //   compressive curve + 40 Hz playback high-pass -2.62 dB
    // i.e. the compressive curve costs 0.75 dB here and the 40 Hz high-pass
    // returns 0.16 dB. No extra boundary tolerance is applied: one wide enough
    // to hold the -2.03 dB reading inside the band would sit on the same knife
    // edge in the other direction.
    //
    // The gate still bites: +1.00 dB above (echo bus too hot) and -3.00 dB
    // below (first repeat lost, current margin 0.38 dB).
    constexpr double hotStimulusCompressionAllowanceDb = 1.0;
    const bool levelOk = referenceLevelMeasured && std::isfinite(referenceLevelDb) &&
                         referenceLevelDb >= -2.0 - hotStimulusCompressionAllowanceDb && referenceLevelDb <= 1.0;
    passed = passed && levelOk;
    if (!levelOk)
        failures.add("first-repeat-level");

    juce::String result;
    result << "EchoPresentationHeadroom: firstRepeat/dry=" << dbString(referenceLevelDb, 3)
           << " hotStimulusBand=[-3,+1]dB (canonical [-2,+1] lives on"
           << " FirstRepeatLevelSpectrum); " << rows << "; 3H uses public All Heads Echo+Reverb with ReverbLevel=-12dB";
    if (!failures.isEmpty())
        result << "; failures=" << failures.joinIntoString(",");
    result << (passed ? " [PASS]" : " [FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::Delay

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
