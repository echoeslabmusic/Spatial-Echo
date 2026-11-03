// TapeRecordSaturator: Stateful record-stage tape saturation with bounded output, sample-rate-normalized memory, and
//   per-frame smoothing.
// Realtime-safety: No allocation, locks, I/O, or host calls in process after prepare()

#pragma once

#include <JuceHeader.h>
#include "dependencies/PerSampleOversampler.h"
#include <vector>

namespace echoes
{

class TapeRecordSaturator
{
public:
    void prepare(double sampleRate, int channels);
    void reset() noexcept;

    void setCharacter(float drive, float saturation, float bias) noexcept;
    void setMakeupGainDb(float makeupGainDb) noexcept;

    // One call is one complete mono frame. Stereo callers must use
    // processStereoFrame() so parameter smoothing advances once per frame.
    float processMono(float inputSample, int channel = 0) noexcept;
    void processStereoFrame(float& left, float& right) noexcept;

private:
    struct ChannelState
    {
        void reset() noexcept;

        float remanence = 0.0f;
        PerSampleOversampler4x oversampler4x;
        PerSampleOversampler8x oversampler8x;
    };

    struct FrameParameters
    {
        // Knee blend of the record transfer, 0 = earliest knee (n = 2),
        // 1 = latest knee (n = 8). See TapeRecordSaturator.cpp.
        float kneeBlend = 0.5f;
        float excitationGain = 1.0f;
        float memoryAmount = 0.0f;
        float stateDecay = 0.0f;
        float biasAmount = 0.0f;
        float makeupLinear = 1.0f;
        float outputLimit = 1.0f;
    };

    void advanceFrameParameters() noexcept;
    float processWithCurrentFrame(float inputSample, int channel) noexcept;
    float processOversampled(float inputSample, ChannelState& state, const FrameParameters& parameters) noexcept;

    double currentSampleRate = 48000.0;
    int oversampleFactor = 8;
    bool hasProcessedAudio = false;

    float targetDrive = 0.0f;
    float targetSaturation = 0.0f;
    float targetBias = 0.0f;
    float targetMakeupGainDb = 0.0f;

    std::vector<ChannelState> channelStates;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> saturationSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> biasSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> makeupGainDbSmoother;
    FrameParameters currentFrameParameters;
};

}  // namespace echoes
