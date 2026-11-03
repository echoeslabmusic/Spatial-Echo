// Purpose: final output protection with constant host latency; feedback oversamplers keep their existing
//   fractional-latency behavior.
// Realtime-safety: prepare/release allocate; process/pushBypassInput do not.
#pragma once

#include "dependencies/PerSampleOversampler.h"
#include <array>
#include <cmath>
#include <vector>

class OutputProtectionStage
{
public:
    // Pinned JUCE 8.0.12: 2x high-quality IIR plus integer correction.
    static constexpr int latencySamples = 4;

    // The oversampler's half-band filters are rate-independent, so only the
    // channel count matters here.
    void prepare(int numChannels)
    {
        channels.clear();
        channels.resize(static_cast<size_t>(std::max(1, numChannels)));
        for (auto& channel : channels)
        {
            channel.oversampler.prepare(true);
            jassert(std::abs(channel.oversampler.getLatencyInSamples() - latencySamples) < 0.001f);
        }
    }

    void release() { channels.clear(); }

    // Warm this independent raw-input delay in normal processing too, so
    // entering host bypass cannot emit the preceding wet signal or silence.
    float pushBypassInput(int channelIndex, float input) noexcept
    {
        if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
            return 0.0f;
        auto& channel = channels[static_cast<size_t>(channelIndex)];
        const auto index = channel.bypassIndex;
        const float output = channel.bypassDelay[index];
        channel.bypassDelay[index] = std::isfinite(input) ? input : 0.0f;
        channel.bypassIndex = (index + 1) % static_cast<size_t>(latencySamples);
        return output;
    }

    float process(int channelIndex, float input, float protectionMix) noexcept
    {
        if (channelIndex < 0 || static_cast<size_t>(channelIndex) >= channels.size())
            return 0.0f;
        auto& channel = channels[static_cast<size_t>(channelIndex)];
        input = std::isfinite(input) ? input : 0.0f;
        const auto index = channel.dryIndex;
        const float dry = channel.dryDelay[index];
        channel.dryDelay[index] = input;
        channel.dryIndex = (index + 1) % static_cast<size_t>(latencySamples);

        // Always run the existing nonlinear path, including while disabled.
        // Switching back on must use current history, not a cold/stale filter.
        float wet = channel.oversampler.process(input, shape);
        wet = std::isfinite(wet) ? std::clamp(wet, -1.0f, 1.0f) : 0.0f;
        return dry + protectionMix * (wet - dry);
    }

private:
    friend struct OutputProtectionStageTestAccess;

    static float shape(float x) noexcept
    {
        constexpr float knee = 0.96f;
        x = std::clamp(x, -2.5f, 2.5f);
        if (std::abs(x) <= knee)
            return x;
        const float compressed = knee + (1.0f - knee) * std::tanh((std::abs(x) - knee) / (1.0f - knee));
        return std::copysign(std::min(compressed, 1.0f), x);
    }

    struct Channel
    {
        echoes::PerSampleOversampler2x oversampler;
        std::array<float, latencySamples> dryDelay{}, bypassDelay{};
        size_t dryIndex = 0, bypassIndex = 0;
    };
    std::vector<Channel> channels;
};
