// DelayAreaProbes: Measurement probes for feedback sub-bass accumulation and tempo-sync transition artifacts.

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "TapeTests.h"
#include "../../DSP/EchoesModel.h"
#include "../../DSP/TapeDelay.h"
#include "../../DSP/DeterministicTestMode.h"
#include "../../DSP/TapeFlavors.h"
#include "../../DSP/dependencies/RbjBiquad.h"
#include "../Common/TestUtilities.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace SpatialEchoTests::TapeEmulation
{
namespace
{
constexpr float kSampleRate = 48000.0f;
constexpr int kBlockSize = 256;
constexpr float kMinus12Dbfs = 0.2511886432f;

double dbFromLinear(double value)
{
    return 20.0 * std::log10(std::max(1.0e-12, value));
}

struct DeterministicPinkNoise
{
    uint32_t state = 0xC6F33DBAu;
    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float b3 = 0.0f;
    float b4 = 0.0f;
    float b5 = 0.0f;
    float b6 = 0.0f;

    float nextWhite() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
    }

    float next() noexcept
    {
        const float white = nextWhite();
        b0 = 0.99886f * b0 + white * 0.0555179f;
        b1 = 0.99332f * b1 + white * 0.0750759f;
        b2 = 0.96900f * b2 + white * 0.1538520f;
        b3 = 0.86650f * b3 + white * 0.3104856f;
        b4 = 0.55000f * b4 + white * 0.5329522f;
        b5 = -0.7616f * b5 - white * 0.0168980f;
        const float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
        b6 = white * 0.115926f;
        return pink * 0.11f;
    }
};

std::vector<float> makePinkNoise(int numSamples)
{
    DeterministicPinkNoise generator;
    std::vector<float> samples(static_cast<size_t>(numSamples), 0.0f);
    double energy = 0.0;
    for (float& sample : samples)
    {
        sample = generator.next();
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const double rms = numSamples > 0 ? std::sqrt(energy / numSamples) : 1.0;
    const float scale = static_cast<float>(kMinus12Dbfs / std::max(1.0e-12, rms));
    for (float& sample : samples)
        sample *= scale;
    return samples;
}

void applyWornFeedbackConfig(EchoesModel& model)
{
    const auto worn = TapeFlavors::resolve(static_cast<int>(TapeFlavor::Worn), TapeFlavors::kDriveAnchor);
    int heads[3] = {1, 0, 0};
    model.UpdateParameters(
        /*low*/ 0.5f, /*high*/ 0.5f,
        /*intensity*/ 0.75f, /*delaytime: 0 = 175 ms*/ 0.0f, heads, /*delayenabled*/ 1, /*mix*/ 1.0f,
        /*reverbenabled*/ 0, /*inputlevel gain*/ 1.0f, /*echoSend*/ 1.0f, worn.noiseOn, worn.hissLevel, worn.condition,
        worn.wfDepthParam, worn.wowRateScale, worn.flutterRateScale,
        /*stereoMode*/ 0,
        /*tempoSync*/ false, 0.0f, 0.0f, 0.0f,
        /*binaural*/ false, 0.0f, 0.0f, 1.0f, 100.0f, 150.0f, 2, worn.drive, worn.saturation, worn.bias,
        /*headRatioPreset*/ 0, /*reverbRouting*/ 0, worn.asperityOverride, worn.dropoutOverride, worn.scrapeScale,
        worn.lpfScale,
        /*feedbackCompensation*/ true);
}

std::array<double, 5> measureRepeatBandRmsDb(const std::vector<float>& output, int tailStart, int repeatSamples)
{
    RbjBiquad highPass1;
    RbjBiquad highPass2;
    RbjBiquad lowPass1;
    RbjBiquad lowPass2;
    highPass1.Init(BiquadType::HPF, 40.0f, kSampleRate, 0.70710678f, 0.0f);
    highPass2.Init(BiquadType::HPF, 40.0f, kSampleRate, 0.70710678f, 0.0f);
    lowPass1.Init(BiquadType::LPF, 100.0f, kSampleRate, 0.70710678f, 0.0f);
    lowPass2.Init(BiquadType::LPF, 100.0f, kSampleRate, 0.70710678f, 0.0f);

    std::vector<float> bandLimited(output.size(), 0.0f);
    for (size_t i = 0; i < output.size(); ++i)
    {
        float sample = highPass1.ProcessSample(output[i]);
        sample = highPass2.ProcessSample(sample);
        sample = lowPass1.ProcessSample(sample);
        bandLimited[i] = lowPass2.ProcessSample(sample);
    }

    std::array<double, 5> repeatDb{};
    for (int repeat = 0; repeat < 5; ++repeat)
    {
        const int start = tailStart + repeat * repeatSamples;
        double energy = 0.0;
        for (int i = 0; i < repeatSamples; ++i)
        {
            const double value = bandLimited[static_cast<size_t>(start + i)];
            energy += value * value;
        }
        repeatDb[static_cast<size_t>(repeat)] = dbFromLinear(std::sqrt(energy / repeatSamples));
    }
    return repeatDb;
}

void updateSyncConfig(TapeDelayProcessor& delay, float head1DelayMs)
{
    int heads[3] = {1, 0, 0};
    delay.UpdateParameters(
        /*intensity*/ 0.0f, /*classic delay knob*/ 0.5f, heads, /*delay enabled*/ 1, /*echo send*/ 1.0f,
        /*noise*/ false, 0.0f, 0.0f,
        /*wow/flutter*/ 0.0f, 1.0f, 1.0f,
        /*tempo sync*/ true, head1DelayMs, 300.0f, 450.0f,
        /*stereo*/ 0, /*head ratio*/ 0,
        /*feedback compensation*/ true,
        /*asperity*/ 0.0f, /*dropout*/ 0.0f,
        /*scrape*/ 1.0f, /*LPF scale*/ 1.0f);
}

std::vector<float> renderSyncTransition(bool switchDelay)
{
    constexpr int eventSample = static_cast<int>(2.0f * kSampleRate);
    constexpr int totalSamples = eventSample + 8 * kBlockSize;
    TapeDelayProcessor delay;
    delay.Reset(kSampleRate, 1, kBlockSize);
    updateSyncConfig(delay, 150.0f);

    std::vector<std::vector<float>> block(1, std::vector<float>(static_cast<size_t>(kBlockSize), 0.0f));
    std::vector<float> output(static_cast<size_t>(totalSamples), 0.0f);
    float phase = 0.0f;
    const float phaseIncrement = juce::MathConstants<float>::twoPi * 440.0f / kSampleRate;

    for (int blockStart = 0; blockStart < totalSamples; blockStart += kBlockSize)
    {
        if (blockStart == eventSample)
            updateSyncConfig(delay, switchDelay ? 156.0f : 150.0f);

        for (int sample = 0; sample < kBlockSize; ++sample)
        {
            block[0][static_cast<size_t>(sample)] = kMinus12Dbfs * std::sin(phase);
            phase += phaseIncrement;
            if (phase >= juce::MathConstants<float>::twoPi)
                phase -= juce::MathConstants<float>::twoPi;
        }

        delay.ProcessBuffer(block, kBlockSize);
        std::copy_n(block[0].begin(), kBlockSize, output.begin() + static_cast<std::ptrdiff_t>(blockStart));
    }
    return output;
}

struct TransitionMetrics
{
    double maxAdjacentDelta = 0.0;
    double bandEnergy = 0.0;
};

TransitionMetrics measureTransition(const std::vector<float>& output)
{
    constexpr int eventSample = static_cast<int>(2.0f * kSampleRate);
    constexpr int halfWindow = 240;
    constexpr int windowSize = halfWindow * 2;
    const int start = eventSample - halfWindow;

    TransitionMetrics metrics;
    for (int i = 1; i < windowSize; ++i)
    {
        const double delta = std::abs(static_cast<double>(output[static_cast<size_t>(start + i)]) -
                                      static_cast<double>(output[static_cast<size_t>(start + i - 1)]));
        metrics.maxAdjacentDelta = std::max(metrics.maxAdjacentDelta, delta);
    }

    const double binHz = kSampleRate / windowSize;
    const int firstBin = static_cast<int>(std::ceil(2000.0 / binHz));
    const int lastBin = static_cast<int>(std::floor(8000.0 / binHz));
    for (int bin = firstBin; bin <= lastBin; ++bin)
    {
        double real = 0.0;
        double imag = 0.0;
        for (int n = 0; n < windowSize; ++n)
        {
            const double window = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * n / (windowSize - 1));
            const double phase = -juce::MathConstants<double>::twoPi * bin * n / windowSize;
            const double sample = output[static_cast<size_t>(start + n)] * window;
            real += sample * std::cos(phase);
            imag += sample * std::sin(phase);
        }
        metrics.bandEnergy += real * real + imag * imag;
    }
    return metrics;
}
}  // namespace

juce::String runFeedbackSubBassAccumulation(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    constexpr int excitationSamples = static_cast<int>(2.0f * kSampleRate);
    constexpr int totalSamples = static_cast<int>(6.0f * kSampleRate);
    constexpr int repeatSamples = static_cast<int>(0.175f * kSampleRate);

    const auto excitation = makePinkNoise(excitationSamples);
    EchoesModel model;
    model.Reset(kSampleRate, 2, kBlockSize);
    applyWornFeedbackConfig(model);

    std::vector<std::vector<float>> block(2, std::vector<float>(static_cast<size_t>(kBlockSize), 0.0f));
    std::vector<float> output(static_cast<size_t>(totalSamples), 0.0f);
    bool finite = true;
    for (int blockStart = 0; blockStart < totalSamples; blockStart += kBlockSize)
    {
        for (int sample = 0; sample < kBlockSize; ++sample)
        {
            const int absoluteSample = blockStart + sample;
            const float value =
                absoluteSample < excitationSamples ? excitation[static_cast<size_t>(absoluteSample)] : 0.0f;
            block[0][static_cast<size_t>(sample)] = value;
            block[1][static_cast<size_t>(sample)] = value;
        }
        model.ProcessBuffer(block, kBlockSize);
        for (int sample = 0; sample < kBlockSize; ++sample)
        {
            const float value = block[0][static_cast<size_t>(sample)];
            finite = finite && std::isfinite(value);
            output[static_cast<size_t>(blockStart + sample)] = value;
        }
    }

    const auto repeatDb = measureRepeatBandRmsDb(output, excitationSamples, repeatSamples);
    const double netRiseDb = repeatDb[4] - repeatDb[0];
    const bool targetMet = netRiseDb <= 1.0;

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "FeedbackSubBassAccumulation");
    root->setProperty("schema", "spatial-echo.feedback-sub-bass-accumulation.v1");
    root->setProperty("configuration", "Release; deterministic; 48kHz/256; 175ms; WORN anchor flavor; intensity=0.75; "
                                       "H1 only; 2s -12dBFS RMS deterministic pink noise then silence; 6s render");
    root->setProperty("band_hz", "40-100");
    root->setProperty("band_method", "cascaded 4th-order Butterworth HP40 + LP100, continuous over render");
    juce::Array<juce::var> repeats;
    for (int repeat = 0; repeat < 5; ++repeat)
        repeats.add(repeatDb[static_cast<size_t>(repeat)]);
    root->setProperty("repeat_band_rms_dbfs", repeats);
    root->setProperty("repeat_1_to_5_net_rise_db", netRiseDb);
    root->setProperty("target_max_rise_db", 1.0);
    root->setProperty("target_met", targetMet);
    root->setProperty("finite", finite);

    const juce::File jsonFile = artifactDirectory().getChildFile("feedback_sub_bass_accumulation.json");
    jsonFile.deleteFile();
    const bool wroteJson = jsonFile.replaceWithText(juce::JSON::toString(juce::var(root), true));

    juce::String result;
    result << "FeedbackSubBassAccumulation: r1=" << juce::String(repeatDb[0], 3)
           << "dBFS r2=" << juce::String(repeatDb[1], 3) << " r3=" << juce::String(repeatDb[2], 3)
           << " r4=" << juce::String(repeatDb[3], 3) << " r5=" << juce::String(repeatDb[4], 3)
           << " net=" << juce::String(netRiseDb, 3) << "dB targetMet=" << (targetMet ? "yes" : "no") << " "
           << ((finite && wroteJson) ? "[PASS]" : "[FAIL]") << "\n"
           << "[INFO] metrics: " << jsonFile.getFullPathName();
    return result;
}

juce::String runSyncCrossfadeThresholdProbe(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    const auto controlOutput = renderSyncTransition(false);
    const auto switchedOutput = renderSyncTransition(true);
    const auto control = measureTransition(controlOutput);
    const auto switched = measureTransition(switchedOutput);

    const double bandExcessDb =
        10.0 * std::log10(std::max(1.0e-24, switched.bandEnergy) / std::max(1.0e-24, control.bandEnergy));
    const double adjacentRatio = switched.maxAdjacentDelta / std::max(1.0e-12, control.maxAdjacentDelta);
    const bool targetMet = bandExcessDb <= 6.0;
    const bool finite = std::isfinite(bandExcessDb) && std::isfinite(adjacentRatio) &&
                        std::isfinite(control.maxAdjacentDelta) && std::isfinite(switched.maxAdjacentDelta);

    auto* root = new juce::DynamicObject();
    root->setProperty("test", "SyncCrossfadeThresholdProbe");
    root->setProperty("schema", "spatial-echo.sync-crossfade-threshold-probe.v1");
    root->setProperty("configuration", "Release; 48kHz/256; H1 tempo-sync; 150ms to 156ms block-boundary change; "
                                       "sustained 440Hz sine at -12dBFS peak; intensity=0; modulation/noise off");
    root->setProperty("transition_window_samples", 480);
    root->setProperty("transition_band_hz", "2000-8000");
    root->setProperty("control_max_adjacent_delta", control.maxAdjacentDelta);
    root->setProperty("switched_max_adjacent_delta", switched.maxAdjacentDelta);
    root->setProperty("adjacent_delta_ratio", adjacentRatio);
    root->setProperty("control_transition_band_energy", control.bandEnergy);
    root->setProperty("switched_transition_band_energy", switched.bandEnergy);
    root->setProperty("transition_band_excess_db", bandExcessDb);
    root->setProperty("band_energy_rule_db", 6.0);
    root->setProperty("target_met", targetMet);
    root->setProperty("adjacent_delta_ratio_rule", 2.0);
    root->setProperty("finite", finite);

    const juce::File jsonFile = artifactDirectory().getChildFile("sync_crossfade_threshold_probe.json");
    jsonFile.deleteFile();
    const bool wroteJson = jsonFile.replaceWithText(juce::JSON::toString(juce::var(root), true));

    juce::String result;
    result << "SyncCrossfadeThresholdProbe: bandExcess=" << juce::String(bandExcessDb, 3)
           << "dB adjacentRatio=" << juce::String(adjacentRatio, 3)
           << " controlDelta=" << juce::String(control.maxAdjacentDelta, 7)
           << " switchedDelta=" << juce::String(switched.maxAdjacentDelta, 7)
           << " targetMet=" << (targetMet ? "yes" : "no") << " " << ((finite && wroteJson) ? "[PASS]" : "[FAIL]")
           << "\n"
           << "[INFO] metrics: " << jsonFile.getFullPathName();
    return result;
}

juce::String runSyncCrossfadeRetargetTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    bool passed = true;
    int cases = 0;
    float maxPrematureChange = 0.0f, maxPartitionDelta = 0.0f;
    double minSettledDifference = std::numeric_limits<double>::infinity();
    double maxRequestedTargetRelativeError = 0.0;
    for (float fs : {48000.0f, 96000.0f})
    {
        const int start = static_cast<int>(fs * 0.350f);
        const int retarget = start + static_cast<int>(fs * 0.007f);
        const int total = static_cast<int>(fs * 0.430f);
        for (int channels : {1, 2})
        {
            const auto render =
                [&](float finalDelay, int blockSize, bool destinationCheck = false, bool fixedDestination = false)
            {
                const int renderSamples = destinationCheck ? static_cast<int>(fs * 0.850f) : total;
                TapeDelayProcessor delay;
                delay.Reset(fs, channels, blockSize);
                int heads[3] = {1, 0, 0};
                const auto setDelay = [&](float delayMs)
                {
                    delay.UpdateParameters(destinationCheck ? 0.0f : 0.65f, 0.0f, heads, 1, 1.0f, false, 0.0f, 0.0f,
                                           0.0f, 0.0f, 0.0f, true, delayMs, delayMs * 2.0f, delayMs * 3.0f, 0, 0, false,
                                           0.0f, 0.0f, 0.0f, 1.0f);
                };
                setDelay(fixedDestination ? finalDelay : 175.0f);
                std::vector<std::vector<float>> output(static_cast<size_t>(channels),
                                                       std::vector<float>(static_cast<size_t>(renderSamples), 0.0f));
                std::vector<std::vector<float>> block(static_cast<size_t>(channels),
                                                      std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
                for (int offset = 0; offset < renderSamples;)
                {
                    if (!fixedDestination && offset == start)
                        setDelay(250.0f);
                    if (!fixedDestination && offset == retarget)
                        setDelay(finalDelay);
                    const int nextEvent = offset < start ? start : offset < retarget ? retarget : renderSamples;
                    const int count = std::min(blockSize, nextEvent - offset);
                    for (int channel = 0; channel < channels; ++channel)
                        for (int sample = 0; sample < count; ++sample)
                            // These periods distinguish all requested delays;
                            // an 8 ms error cannot hide near a full sine cycle.
                            block[static_cast<size_t>(channel)][static_cast<size_t>(sample)] = static_cast<float>(
                                0.05 * std::sin(6.283185307179586 * (31.7 + channel * 15.6) * (offset + sample) / fs));
                    delay.ProcessBuffer(block, count);
                    for (int channel = 0; channel < channels; ++channel)
                        std::copy_n(block[static_cast<size_t>(channel)].begin(), count,
                                    output[static_cast<size_t>(channel)].begin() + offset);
                    offset += count;
                }
                return output;
            };
            const auto uninterrupted = render(250.0f, 127);
            // Cover a new large destination, reversal, and a small target edit
            // during an already-active large transition.
            for (float destination : {300.0f, 175.0f, 258.0f})
            {
                ++cases;
                const auto queued = render(destination, 127);
                const auto singleSample = render(destination, 1);
                double settledDifference = 0.0;
                for (int channel = 0; channel < channels; ++channel)
                {
                    for (int sample = 0; sample < total; ++sample)
                    {
                        passed =
                            passed &&
                            std::isfinite(queued[static_cast<size_t>(channel)][static_cast<size_t>(sample)]) &&
                            std::isfinite(singleSample[static_cast<size_t>(channel)][static_cast<size_t>(sample)]) &&
                            std::isfinite(uninterrupted[static_cast<size_t>(channel)][static_cast<size_t>(sample)]);
                        maxPartitionDelta =
                            std::max(maxPartitionDelta,
                                     std::abs(queued[static_cast<size_t>(channel)][static_cast<size_t>(sample)] -
                                              singleSample[static_cast<size_t>(channel)][static_cast<size_t>(sample)]));
                        if (sample >= retarget && sample < retarget + static_cast<int>(fs * 0.004f))
                            maxPrematureChange = std::max(
                                maxPrematureChange,
                                std::abs(queued[static_cast<size_t>(channel)][static_cast<size_t>(sample)] -
                                         uninterrupted[static_cast<size_t>(channel)][static_cast<size_t>(sample)]));
                        if (sample >= total - static_cast<int>(fs * 0.020f))
                        {
                            const double delta =
                                queued[static_cast<size_t>(channel)][static_cast<size_t>(sample)] -
                                uninterrupted[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
                            settledDifference += delta * delta;
                        }
                    }
                }
                minSettledDifference = std::min(minSettledDifference, settledDifference);

                // A difference from 250 ms alone does not establish arrival at
                // the requested destination. Remove regeneration history and
                // compare the settled waveform to an independently initialized
                // processor held at that requested delay from its first sample.
                const auto settledQueued = render(destination, 127, true, false);
                const auto requestedReference = render(destination, 127, true, true);
                const int settledStart = static_cast<int>(fs * 0.800f);
                for (int channel = 0; channel < channels; ++channel)
                {
                    double errorEnergy = 0.0, referenceEnergy = 0.0;
                    for (size_t sample = static_cast<size_t>(settledStart);
                         sample < settledQueued[static_cast<size_t>(channel)].size(); ++sample)
                    {
                        const double actual = settledQueued[static_cast<size_t>(channel)][sample];
                        const double expected = requestedReference[static_cast<size_t>(channel)][sample];
                        passed = passed && std::isfinite(actual) && std::isfinite(expected);
                        errorEnergy += (actual - expected) * (actual - expected);
                        referenceEnergy += expected * expected;
                    }
                    const double relativeError = std::sqrt(errorEnergy / std::max(1.0e-20, referenceEnergy));
                    passed = passed && std::isfinite(relativeError) && referenceEnergy > 1.0e-6;
                    maxRequestedTargetRelativeError = std::max(maxRequestedTargetRelativeError, relativeError);
                }
            }
        }
    }
    passed = passed && maxPrematureChange <= 1.0e-7f && maxPartitionDelta <= 1.0e-6f &&
             std::isfinite(minSettledDifference) && minSettledDifference > 1.0e-6 &&
             cases == 12
             // The inherited float one-pole glide has finite timing resolution;
             // allow its small residual while rejecting each wrong destination.
             && maxRequestedTargetRelativeError <= 0.05;
    return "SyncCrossfadeRetarget: prematureChange=" + juce::String(maxPrematureChange, 9) +
           " partitionDelta=" + juce::String(maxPartitionDelta, 9) +
           " minSettledDifference=" + juce::String(minSettledDifference, 7) +
           " requestedTargetRelativeError=" + juce::String(maxRequestedTargetRelativeError, 7) +
           " cases=" + juce::String(cases) + (passed ? " [PASS]" : " [FAIL]");
}

juce::String runTapeSpliceContinuityTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    constexpr std::array<float, 3> sampleRates{44100.0f, 48000.0f, 96000.0f};
    constexpr float depth = 0.36f;
    constexpr float spliceWidth = 0.016f;
    constexpr double pi = 3.14159265358979323846;
    bool passed = SpatialEchoDeterminism::isDeterministicTestMode();
    float worstStep = 0.0f, worstPartitionDelta = 0.0f, worstCaptureDelta = 0.0f;
    juce::String rows;

    for (float fs : sampleRates)
    {
        for (int channels : {1, 2})
        {
            // Both transport modes cross a full revolution with audible audio
            // and regeneration present at the second splice.
            for (bool synced : {false, true})
            {
                const float headDelayMs = synced ? 20.0f : 70.0f;
                const double loopSamples = static_cast<double>(fs) * headDelayMs * 0.001 * 17.14;
                const int totalSamples = static_cast<int>(std::ceil(loopSamples * 1.04));
                std::vector<std::vector<float>> reference(static_cast<size_t>(channels),
                                                          std::vector<float>(static_cast<size_t>(totalSamples), 0.0f));
                std::vector<float> referenceNoise(static_cast<size_t>(totalSamples), 0.0f);
                float maxStep = 0.0f, minimumGain = 1.0f, previousGain = 1.0f;
                double dipSum = 0.0;
                int dipSamples = 0, gainSamples = 0;
                bool rowFinite = true;

                for (int blockSize : {1, 257})
                {
                    TapeDelayProcessor delay;
                    delay.Reset(fs, channels, blockSize);
                    int heads[3] = {1, 1, 1};
                    delay.UpdateParameters(0.82f, 1.0f, heads, 1, 1.0f, true, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, synced,
                                           headDelayMs, headDelayMs * 2.0f, headDelayMs * 3.0f, 0, 0, true, 0.0f, 0.0f,
                                           0.0f, 1.0f);
                    delay.enablePerHeadCapture(true);
                    std::vector<std::vector<float>> block(static_cast<size_t>(channels),
                                                          std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
                    for (int offset = 0; offset < totalSamples; offset += blockSize)
                    {
                        const int count = std::min(blockSize, totalSamples - offset);
                        for (int channel = 0; channel < channels; ++channel)
                            for (int sample = 0; sample < count; ++sample)
                                block[static_cast<size_t>(channel)][static_cast<size_t>(sample)] = static_cast<float>(
                                    0.05 * std::sin(2.0 * pi * (173.0 + 37.0 * channel) * (offset + sample) / fs));
                        delay.ProcessBuffer(block, count);
                        const auto& noise = delay.getOutputNoiseBuffer();
                        for (int sample = 0; sample < count; ++sample)
                        {
                            const size_t index = static_cast<size_t>(offset + sample);
                            for (int channel = 0; channel < channels; ++channel)
                            {
                                const float output = block[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
                                const float capture = delay.getHeadOutput(0, channel)[sample] +
                                                      delay.getHeadOutput(1, channel)[sample] +
                                                      delay.getHeadOutput(2, channel)[sample];
                                rowFinite = rowFinite && std::isfinite(output) && std::isfinite(capture);
                                worstCaptureDelta = std::max(worstCaptureDelta, std::abs(output - capture));
                                if (blockSize == 1)
                                    reference[static_cast<size_t>(channel)][index] = output;
                                else
                                    worstPartitionDelta =
                                        std::max(worstPartitionDelta,
                                                 std::abs(output - reference[static_cast<size_t>(channel)][index]));
                            }
                            rowFinite = rowFinite && std::isfinite(noise[static_cast<size_t>(sample)]);
                            if (blockSize == 1)
                                referenceNoise[index] = noise[static_cast<size_t>(sample)];
                            else
                                worstPartitionDelta =
                                    std::max(worstPartitionDelta,
                                             std::abs(noise[static_cast<size_t>(sample)] - referenceNoise[index]));
                        }
                        if (blockSize == 1)
                        {
                            // The scalar is the current raw output bed; the
                            // captured sample has the actual playback tape gain.
                            // Their ratio observes production without replacing
                            // its splice algorithm with a test-only surrogate.
                            const float rawNoise = delay.getOutputNoise();
                            if (std::abs(rawNoise) > 1.0e-10f)
                            {
                                const float gain = noise[0] / rawNoise;
                                rowFinite = rowFinite && std::isfinite(gain);
                                maxStep = std::max(maxStep, std::abs(gain - previousGain));
                                previousGain = gain;
                                minimumGain = std::min(minimumGain, gain);
                                ++gainSamples;
                                if (gain < 0.99999f)
                                {
                                    dipSum += gain;
                                    ++dipSamples;
                                }
                            }
                        }
                    }
                }
                const double meanDip = dipSamples > 0 ? dipSum / dipSamples : 1.0;
                const double stepLimit = 2.0 * depth * pi / (spliceWidth * loopSamples);
                const bool rowPassed = rowFinite && gainSamples > totalSamples * 0.99 && dipSamples > 100 &&
                                       std::abs(minimumGain - (1.0f - depth)) < 0.001f &&
                                       std::abs(meanDip - (1.0 - 0.5 * depth)) < 0.01 && maxStep < stepLimit;
                passed = passed && rowPassed;
                worstStep = std::max(worstStep, maxStep);
                rows << (rows.isEmpty() ? "" : "; ") << static_cast<int>(fs) << "/" << channels
                     << (synced ? "/sync" : "/free") << " step=" << juce::String(maxStep, 6)
                     << " min=" << juce::String(minimumGain, 4) << " mean=" << juce::String(meanDip, 4)
                     << (rowPassed ? " ok" : " FAIL");
            }
        }
    }
    passed = passed && worstPartitionDelta <= 1.0e-6f && worstCaptureDelta <= 1.0e-7f;
    return "TapeSpliceContinuity: maxStep=" + juce::String(worstStep, 6) +
           " partitionDelta=" + juce::String(worstPartitionDelta, 9) +
           " captureDelta=" + juce::String(worstCaptureDelta, 9) + "; " + rows + (passed ? " [PASS]" : " [FAIL]");
}

juce::String runTapeNoiseTransitionTest(SpatialEchoAudioProcessor& processor)
{
    juce::ignoreUnused(processor);
    constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};
    const auto readTaps = [](const TapeNoiseProcessor& noise)
    {
        return std::array<float, 5>{noise.getLoopNoise(), noise.getOutputNoiseForChannel(0),
                                    noise.getOutputNoiseForChannel(1), noise.getReverbNoiseForChannel(0),
                                    noise.getReverbNoiseForChannel(1)};
    };
    bool passed = true;
    double worstRatioError = 0.0;
    juce::String rows;
    for (float fs : sampleRates)
    {
        for (bool disableSwitch : {false, true})
        {
            TapeNoiseProcessor fading, continuing;
            for (auto* noise : {&fading, &continuing})
            {
                noise->reset(fs);
                noise->setDeterministicSeed(0xA770FFu);
                noise->updateParameters(true, 1.0f, 1.0f, 1.0f, -1.0f, 0.0f);
            }
            for (int sample = 0; sample < static_cast<int>(fs * 0.5f); ++sample)
            {
                fading.processSample(0.3f);
                continuing.processSample(0.3f);
            }
            fading.updateParameters(!disableSwitch, disableSwitch ? 1.0f : 0.0f, 1.0f, 1.0f, -1.0f, 0.0f);
            bool finite = true, exactSilence = true;
            int measuredRatios = 0;
            double expectedRatio = 1.0;
            double rowRatioError = 0.0;
            const double decay = 1.0 - TapeNoiseRate::smoothingCoeff(0.001f, fs);
            const int fadeSamples = static_cast<int>(fs * 0.5f);
            for (int sample = 0; sample < fadeSamples; ++sample)
            {
                fading.processSample(0.3f);
                continuing.processSample(0.3f);
                expectedRatio *= decay;
                const auto offTaps = readTaps(fading);
                const auto onTaps = readTaps(continuing);
                for (size_t tap = 0; tap < offTaps.size(); ++tap)
                {
                    finite = finite && std::isfinite(offTaps[tap]);
                    if (sample < static_cast<int>(fs * 0.05f) && std::abs(onTaps[tap]) > 1.0e-9f)
                    {
                        rowRatioError = std::max(
                            rowRatioError, std::abs(static_cast<double>(offTaps[tap]) / onTaps[tap] - expectedRatio));
                        ++measuredRatios;
                    }
                    if (sample >= fadeSamples - 256)
                        exactSilence = exactSilence && offTaps[tap] == 0.0f;
                }
            }
            // Returning from settled silence starts at zero gain and recovers
            // the complete bed rather than exposing a stale full-level sample.
            fading.updateParameters(true, 1.0f, 1.0f, 1.0f, -1.0f, 0.0f);
            std::array<double, 5> earlyEnergy{}, lateEnergy{};
            for (int sample = 0; sample < static_cast<int>(fs * 0.1f); ++sample)
            {
                fading.processSample(0.3f);
                const auto taps = readTaps(fading);
                for (size_t tap = 0; tap < taps.size(); ++tap)
                {
                    finite = finite && std::isfinite(taps[tap]);
                    const double energy = static_cast<double>(taps[tap]) * taps[tap];
                    if (sample < static_cast<int>(fs * 0.005f))
                        earlyEnergy[tap] += energy;
                    if (sample >= static_cast<int>(fs * 0.095f))
                        lateEnergy[tap] += energy;
                }
            }
            bool recovered = true;
            for (size_t tap = 0; tap < earlyEnergy.size(); ++tap)
                recovered = recovered && lateEnergy[tap] > earlyEnergy[tap] * 8.0 && lateEnergy[tap] > 1.0e-14;
            const bool rowPassed = finite && exactSilence && recovered &&
                                   measuredRatios > static_cast<int>(fs * 0.2f) && rowRatioError < 0.001;
            passed = passed && rowPassed;
            worstRatioError = std::max(worstRatioError, rowRatioError);
            rows << (rows.isEmpty() ? "" : "; ") << static_cast<int>(fs) << (disableSwitch ? "/switch" : "/amount")
                 << " decayError=" << juce::String(rowRatioError, 6) << " silence=" << (exactSilence ? "yes" : "no")
                 << " recovered=" << (recovered ? "yes" : "no") << (rowPassed ? " ok" : " FAIL");
        }
    }
    return "TapeNoiseTransition: maxDecayError=" + juce::String(worstRatioError, 6) + "; " + rows +
           (passed ? " [PASS]" : " [FAIL]");
}

}  // namespace SpatialEchoTests::TapeEmulation

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
