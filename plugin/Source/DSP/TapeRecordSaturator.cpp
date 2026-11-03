// TapeRecordSaturator: Implements a first-party tape-record saturator with finite output, silence decay, exact reset,
//   and stereo-independent state.
// Realtime-safety: No allocation, locks, or I/O in process after prepare()

#include "TapeRecordSaturator.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace echoes
{
namespace
{
constexpr float kInputClamp = 32.0f;
constexpr float kStateFlushThreshold = 1.0e-12f;
constexpr float kSilenceThreshold = 1.0e-9f;
constexpr float kMinimumMakeupGainDb = -24.0f;
constexpr float kMaximumMakeupGainDb = 24.0f;
// Record-flux ceiling at unity makeup.  The transfer below is normalized by
// this ceiling and keeps the derivative at the origin exactly one, so ordinary
// first repeats retain their calibrated small-signal level while deliberate
// self-oscillation approaches a lower, loop-owned plateau.
constexpr float kRecordFluxCeiling = 0.70f;
constexpr double kParameterSmoothingSeconds = 0.01;

float clamp01(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    return juce::jlimit(0.0f, 1.0f, value);
}

float sanitizeFinite(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;

    return juce::jlimit(-kInputClamp, kInputClamp, value);
}

float normalizedTanh(float value, float normalizer) noexcept
{
    const float safeNormalizer = std::max(1.0f, normalizer);
    return std::tanh(value) / safeNormalizer;
}
}  // namespace

void TapeRecordSaturator::ChannelState::reset() noexcept
{
    remanence = 0.0f;
    oversampler4x.reset();
    oversampler8x.reset();
}

void TapeRecordSaturator::prepare(double sampleRate, int channels)
{
    currentSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    oversampleFactor = currentSampleRate <= 48000.0 ? 8 : 4;

    const int allocatedChannels = std::max(2, channels);
    channelStates.clear();
    channelStates.resize(static_cast<size_t>(allocatedChannels));

    for (auto& state : channelStates)
    {
        state.oversampler4x.prepare();
        state.oversampler8x.prepare();
        state.reset();
    }

    driveSmoother.reset(currentSampleRate, kParameterSmoothingSeconds);
    saturationSmoother.reset(currentSampleRate, kParameterSmoothingSeconds);
    biasSmoother.reset(currentSampleRate, kParameterSmoothingSeconds);
    makeupGainDbSmoother.reset(currentSampleRate, kParameterSmoothingSeconds);

    driveSmoother.setCurrentAndTargetValue(clamp01(targetDrive));
    saturationSmoother.setCurrentAndTargetValue(clamp01(targetSaturation));
    biasSmoother.setCurrentAndTargetValue(clamp01(targetBias));
    makeupGainDbSmoother.setCurrentAndTargetValue(targetMakeupGainDb);

    advanceFrameParameters();
    hasProcessedAudio = false;
}

void TapeRecordSaturator::reset() noexcept
{
    for (auto& state : channelStates)
        state.reset();

    driveSmoother.setCurrentAndTargetValue(clamp01(targetDrive));
    saturationSmoother.setCurrentAndTargetValue(clamp01(targetSaturation));
    biasSmoother.setCurrentAndTargetValue(clamp01(targetBias));
    makeupGainDbSmoother.setCurrentAndTargetValue(targetMakeupGainDb);

    advanceFrameParameters();
    hasProcessedAudio = false;
}

void TapeRecordSaturator::setCharacter(float drive, float saturation, float bias) noexcept
{
    targetDrive = std::isfinite(drive) ? clamp01(drive) : targetDrive;
    targetSaturation = std::isfinite(saturation) ? clamp01(saturation) : targetSaturation;
    targetBias = std::isfinite(bias) ? clamp01(bias) : targetBias;

    if (!hasProcessedAudio)
    {
        driveSmoother.setCurrentAndTargetValue(targetDrive);
        saturationSmoother.setCurrentAndTargetValue(targetSaturation);
        biasSmoother.setCurrentAndTargetValue(targetBias);
        advanceFrameParameters();
        return;
    }

    driveSmoother.setTargetValue(targetDrive);
    saturationSmoother.setTargetValue(targetSaturation);
    biasSmoother.setTargetValue(targetBias);
}

void TapeRecordSaturator::setMakeupGainDb(float makeupGainDb) noexcept
{
    targetMakeupGainDb =
        juce::jlimit(kMinimumMakeupGainDb, kMaximumMakeupGainDb, std::isfinite(makeupGainDb) ? makeupGainDb : 0.0f);

    if (!hasProcessedAudio)
    {
        makeupGainDbSmoother.setCurrentAndTargetValue(targetMakeupGainDb);
        advanceFrameParameters();
        return;
    }

    makeupGainDbSmoother.setTargetValue(targetMakeupGainDb);
}

float TapeRecordSaturator::processMono(float inputSample, int channel) noexcept
{
    if (channelStates.empty())
        return sanitizeFinite(inputSample);

    const int clampedChannel = juce::jlimit(0, static_cast<int>(channelStates.size()) - 1, channel);
    advanceFrameParameters();

    hasProcessedAudio = true;
    return processWithCurrentFrame(inputSample, clampedChannel);
}

void TapeRecordSaturator::processStereoFrame(float& left, float& right) noexcept
{
    if (channelStates.empty())
    {
        left = sanitizeFinite(left);
        right = sanitizeFinite(right);
        return;
    }

    advanceFrameParameters();
    hasProcessedAudio = true;
    left = processWithCurrentFrame(left, 0);
    right = processWithCurrentFrame(right, std::min(1, static_cast<int>(channelStates.size()) - 1));
}

void TapeRecordSaturator::advanceFrameParameters() noexcept
{
    const float drive = clamp01(driveSmoother.getNextValue());
    const float saturation = clamp01(saturationSmoother.getNextValue());
    const float bias = clamp01(biasSmoother.getNextValue());
    const float nextMakeupGainDb = makeupGainDbSmoother.getNextValue();
    const float makeupGainDb = juce::jlimit(kMinimumMakeupGainDb, kMaximumMakeupGainDb,
                                            std::isfinite(nextMakeupGainDb) ? nextMakeupGainDb : 0.0f);

    const float baseShapeAmount =
        7.0f * std::pow(drive, 1.1f) * (0.3f + 0.7f * saturation) + 2.0f * saturation * saturation;
    // Higher bias is the cleaner operating point; under-biasing makes the
    // odd-order knee arrive earlier. Keep the direct asymmetric term subtle:
    // a record stage must not turn normal two-tone material into a dominant
    // difference tone before the feedback loop has done any work.
    const float shapeAmount = baseShapeAmount * (1.12f - 0.24f * bias);
    // Shape amount is a KNEE POSITION, never a gain. More drive/saturation (and
    // less bias) pulls the knee earlier; a clean, well-biased stage stays
    // linear almost to the flux ceiling. kKneeReference is set so the shipping
    // flavour anchors land near a 0.5 blend.
    constexpr float kKneeReference = 3.0f;
    currentFrameParameters.kneeBlend =
        juce::jlimit(0.05f, 1.0f, 1.0f / (1.0f + std::max(0.0f, shapeAmount) / kKneeReference));
    currentFrameParameters.excitationGain = 1.0f + 4.5f * drive + 2.5f * saturation;
    currentFrameParameters.memoryAmount = (0.03f + 0.52f * saturation * saturation) * (0.35f + 0.65f * drive);
    currentFrameParameters.biasAmount = bias * (0.002f + 0.018f * saturation);
    currentFrameParameters.makeupLinear = juce::Decibels::decibelsToGain(makeupGainDb, -60.0f);
    currentFrameParameters.outputLimit = std::max(1.0f, currentFrameParameters.makeupLinear);

    const float normalizedCharacter = 0.4f * drive + 0.6f * saturation;
    const float tauMs = juce::jmap(normalizedCharacter, 7.0f, 55.0f);
    const double oversampledRate = currentSampleRate * static_cast<double>(oversampleFactor);
    const double tauSamples = std::max(1.0, 0.001 * static_cast<double>(tauMs) * oversampledRate);
    currentFrameParameters.stateDecay = static_cast<float>(std::exp(-1.0 / tauSamples));

    const bool parametersAreFinite =
        std::isfinite(currentFrameParameters.kneeBlend) && std::isfinite(currentFrameParameters.excitationGain) &&
        std::isfinite(currentFrameParameters.memoryAmount) && std::isfinite(currentFrameParameters.stateDecay) &&
        std::isfinite(currentFrameParameters.biasAmount) && std::isfinite(currentFrameParameters.makeupLinear) &&
        std::isfinite(currentFrameParameters.outputLimit);

    if (!parametersAreFinite)
        currentFrameParameters = FrameParameters{};
}

float TapeRecordSaturator::processWithCurrentFrame(float inputSample, int channel) noexcept
{
    ChannelState& state = channelStates[static_cast<size_t>(channel)];
    const float sanitizedInput = sanitizeFinite(inputSample);

    const auto processor = [this, &state](float oversampledInput) noexcept
    { return processOversampled(oversampledInput, state, currentFrameParameters); };

    if (oversampleFactor >= 8)
        return state.oversampler8x.process(sanitizedInput, processor);

    return state.oversampler4x.process(sanitizedInput, processor);
}

float TapeRecordSaturator::processOversampled(float inputSample, ChannelState& state,
                                              const FrameParameters& parameters) noexcept
{
    const float input = sanitizeFinite(inputSample);
    if (!std::isfinite(state.remanence))
        state.remanence = 0.0f;

    const float source = normalizedTanh(parameters.excitationGain * input, parameters.excitationGain);
    const float stateTarget = juce::jlimit(-1.0f, 1.0f, source + parameters.biasAmount * input * std::abs(input));

    state.remanence = parameters.stateDecay * state.remanence + (1.0f - parameters.stateDecay) * stateTarget;

    if (std::abs(state.remanence) < kStateFlushThreshold && std::abs(input) < kSilenceThreshold)
        state.remanence = 0.0f;

    const float biasedInput = input + parameters.biasAmount * input * input;
    const float recordInput = biasedInput + parameters.memoryAmount * state.remanence;

    // Record transfer: x / (1 + |x/C|^n)^(1/n), blended between n = 2 and
    // n = 8 so the knee position stays continuously controllable.
    //
    // This shape is the qualitative anhysteretic magnetisation curve of biased
    // tape: unit slope at the origin, monotonically COMPRESSIVE above it, hard
    // asymptote at the flux ceiling C. The Jiles-Atherton law every serious
    // tape model uses (M = M_s * L(Q), L = coth Q - 1/Q) expands as
    // L(q) = q/3 - q^3/45 + ..., so its third-order term is NEGATIVE.
    //
    // The obvious alternative, `C * tanh((x + a*x^3)/C)` with a ~= 2.4-3.0, is
    // wrong here: its small-signal gain is 1 + (a - 1/(3C^2))*x^2 ~= 1 + 2.3*x^2,
    // i.e. it EXPANDS: measured +0.14 dB at -20 dBFS, +1.24 dB at -9 dBFS, and
    // +1.47 dB at -6 dBFS before the ceiling takes over. Inside the
    // regeneration loop that is a positive-feedback mechanism - loop gain rises
    // with level, so at the top of the Feedback knob the tail ACCELERATES
    // (measured +0.08 dB/repeat over repeats 2-8 rising to +0.75 dB/repeat
    // over repeats 20-40, peaking at 1.55 full-scale by repeat 40).
    //
    // Cost: three square roots, one divide and a handful of multiplies, in
    // place of one std::tanh. Both exponents are powers of two on purpose so
    // no std::pow is needed on the audio path (this runs 4x or 8x oversampled).
    // If the late branch ever overflows to infinity its reciprocal is zero, so
    // the blend degrades to the early branch instead of producing a NaN.
    const float normalized = std::abs(recordInput) * (1.0f / kRecordFluxCeiling);
    const float normalizedSquared = normalized * normalized;
    const float earlyKneeDenominator = std::sqrt(1.0f + normalizedSquared);
    const float normalizedEighth = normalizedSquared * normalizedSquared * normalizedSquared * normalizedSquared;
    const float lateKneeDenominator = std::sqrt(std::sqrt(std::sqrt(1.0f + normalizedEighth)));
    const float blendedNumerator =
        (1.0f - parameters.kneeBlend) * lateKneeDenominator + parameters.kneeBlend * earlyKneeDenominator;
    const float shapedGain = blendedNumerator / (earlyKneeDenominator * lateKneeDenominator);
    float output = recordInput * shapedGain * parameters.makeupLinear;

    if (!std::isfinite(output))
    {
        state.remanence = 0.0f;
        output = 0.0f;
    }

    return juce::jlimit(-parameters.outputLimit, parameters.outputLimit, output);
}

}  // namespace echoes
