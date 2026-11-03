// EchoesModelTestAccess: Test-only access to EchoesModel internals (see the header).

#include "EchoesModelTestAccess.h"

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../TapeEmulation/TapeDelayTestAccess.h"
#include <algorithm>

bool EchoesModelTestAccess::getEchoPathPreTrimMetrics(const EchoesModel& model, float& peak, float& rms) noexcept
{
    peak = model.debugEchoPathPreTrimPeak.load(std::memory_order_relaxed);
    rms = model.debugEchoPathPreTrimRms.load(std::memory_order_relaxed);
    return true;
}

void EchoesModelTestAccess::enableEchoPathPreTrimProbe(EchoesModel& model, bool enabled) noexcept
{
    model.debugEchoPathPreTrimProbeEnabled.store(enabled, std::memory_order_relaxed);
}

void EchoesModelTestAccess::resetEchoPathPreTrimProbe(EchoesModel& model) noexcept
{
    model.debugEchoPathPreTrimPeak.store(0.0f, std::memory_order_relaxed);
    model.debugEchoPathPreTrimRms.store(0.0f, std::memory_order_relaxed);
}

float EchoesModelTestAccess::feedbackCompensationGain(const EchoesModel& model) noexcept
{
    return model.TapeDelay ? TapeDelayTestAccess::feedbackCompensationGain(*model.TapeDelay) : 1.0f;
}

float EchoesModelTestAccess::noiseFeedbackIntensity(const EchoesModel& model) noexcept
{
    return model.TapeDelay ? TapeDelayTestAccess::noiseFeedbackIntensity(*model.TapeDelay) : 0.0f;
}

bool EchoesModelTestAccess::getLastBranchBalanceGains(const EchoesModel& model, float& echoGain,
                                                      float& reverbGain) noexcept
{
    if (model.echoGainRamp.empty() || model.reverbGainRamp.empty())
        return false;
    echoGain = model.echoGainRamp.back();
    reverbGain = model.reverbGainRamp.back();
    return true;
}

void EchoesModelTestAccess::enableReverbInputProbe(EchoesModel& model, bool enabled) noexcept
{
    model.debugReverbInputProbeEnabled.store(enabled, std::memory_order_relaxed);
}

void EchoesModelTestAccess::resetReverbInputProbe(EchoesModel& model) noexcept
{
    for (size_t route = 0; route < 3; ++route)
    {
        model.debugReverbInputPeak[route].store(0.0f, std::memory_order_relaxed);
        model.debugReverbInputRms[route].store(0.0f, std::memory_order_relaxed);
    }
}

bool EchoesModelTestAccess::getReverbInputMetrics(const EchoesModel& model, int routingMode, float& peak,
                                                  float& rms) noexcept
{
    if (routingMode < 0 || routingMode >= 3)
        return false;
    const auto index = static_cast<size_t>(routingMode);
    peak = model.debugReverbInputPeak[index].load(std::memory_order_relaxed);
    rms = model.debugReverbInputRms[index].load(std::memory_order_relaxed);
    return true;
}

void EchoesModelTestAccess::enableReverbReturnProbe(EchoesModel& model, bool enabled) noexcept
{
    model.debugReverbReturnProbeEnabled.store(enabled, std::memory_order_relaxed);
}

void EchoesModelTestAccess::resetReverbReturnProbe(EchoesModel& model) noexcept
{
    for (size_t route = 0; route < 3; ++route)
    {
        model.debugReverbReturnPeak[route].store(0.0f, std::memory_order_relaxed);
        model.debugReverbReturnRms[route].store(0.0f, std::memory_order_relaxed);
    }
}

bool EchoesModelTestAccess::getReverbReturnMetrics(const EchoesModel& model, int routingMode, float& peak,
                                                   float& rms) noexcept
{
    if (routingMode < 0 || routingMode >= 3)
        return false;
    const auto index = static_cast<size_t>(routingMode);
    peak = model.debugReverbReturnPeak[index].load(std::memory_order_relaxed);
    rms = model.debugReverbReturnRms[index].load(std::memory_order_relaxed);
    return true;
}

bool EchoesModelTestAccess::copyReverbReturnBuffer(const EchoesModel& model, std::vector<std::vector<float>>& out,
                                                   int numSamples)
{
    const int numChannels = model.NumChannels;
    if (numSamples < 0 || static_cast<int>(model.reverbRecoveredBuffer.size()) < numChannels ||
        static_cast<int>(out.size()) < numChannels)
        return false;

    for (size_t ch = 0; ch < static_cast<size_t>(std::max(0, numChannels)); ++ch)
    {
        if (static_cast<int>(model.reverbRecoveredBuffer[ch].size()) < numSamples ||
            static_cast<int>(out[ch].size()) < numSamples)
            return false;

        std::copy_n(model.reverbRecoveredBuffer[ch].begin(), numSamples, out[ch].begin());
    }

    return true;
}

void EchoesModelTestAccess::setWetBusFault(EchoesModel& model, float amplitude) noexcept
{
    model.debugWetBusFault = amplitude;
}

void EchoesModelTestAccess::poisonTapeLoop(EchoesModel& model, float value)
{
    TapeDelayTestAccess::poisonTapeBuffer(*model.TapeDelay, value);
}

void EchoesModelTestAccess::enableStageTimingCapture(EchoesModel& model, bool enabled) noexcept
{
    model.stageTimingEnabled.store(enabled, std::memory_order_relaxed);
}

void EchoesModelTestAccess::resetStageTimingCapture(EchoesModel& model) noexcept
{
    model.stageTiming.reset();
}

bool EchoesModelTestAccess::getStageTimingSnapshot(const EchoesModel& model,
                                                   EchoesModel::StageTimingSnapshot& out) noexcept
{
    const auto& timing = model.stageTiming;
    out.blocks = timing.blocks.load(std::memory_order_relaxed);
    out.totalUs = timing.totalUs.load(std::memory_order_relaxed);
    out.preampUs = timing.preampUs.load(std::memory_order_relaxed);
    out.reverbUs = timing.reverbUs.load(std::memory_order_relaxed);
    out.delayUs = timing.delayUs.load(std::memory_order_relaxed);
    out.toneUs = timing.toneUs.load(std::memory_order_relaxed);
    out.mixUs = timing.mixUs.load(std::memory_order_relaxed);
    out.noiseUs = timing.noiseUs.load(std::memory_order_relaxed);
    out.binauralUs = timing.binauralUs.load(std::memory_order_relaxed);
    out.maxBlockUs = timing.maxBlockUs.load(std::memory_order_relaxed);
    out.maxPreampUs = timing.maxPreampUs.load(std::memory_order_relaxed);
    out.maxReverbUs = timing.maxReverbUs.load(std::memory_order_relaxed);
    out.maxDelayUs = timing.maxDelayUs.load(std::memory_order_relaxed);
    out.maxToneUs = timing.maxToneUs.load(std::memory_order_relaxed);
    out.maxMixUs = timing.maxMixUs.load(std::memory_order_relaxed);
    out.maxNoiseUs = timing.maxNoiseUs.load(std::memory_order_relaxed);
    out.maxBinauralUs = timing.maxBinauralUs.load(std::memory_order_relaxed);
    return model.stageTimingEnabled.load(std::memory_order_relaxed);
}

void EchoesModelTestAccess::enableHeadTimingCapture(EchoesModel& model, bool enabled)
{
    if (model.TapeDelay)
        TapeDelayTestAccess::enableHeadTimingCapture(*model.TapeDelay, enabled);
}

bool EchoesModelTestAccess::getHeadTimingSnapshot(const EchoesModel& model, TapeDelayProcessor::HeadTimingSnapshot& out)
{
    if (!model.TapeDelay)
        return false;
    return TapeDelayTestAccess::getHeadTimingSnapshot(*model.TapeDelay, out);
}

void EchoesModelTestAccess::processRecordChainOnly(EchoesModel& model, std::vector<std::vector<float>>& buffer,
                                                   int blockSize)
{
    if (model.TapeDelay)
        TapeDelayTestAccess::processRecordChainOnly(*model.TapeDelay, buffer, blockSize);
}

void EchoesModelTestAccess::processPlaybackOnly(EchoesModel& model, std::vector<std::vector<float>>& buffer,
                                                int blockSize)
{
    if (model.TapeDelay)
        TapeDelayTestAccess::processPlaybackOnly(*model.TapeDelay, buffer, blockSize);
}

#endif
