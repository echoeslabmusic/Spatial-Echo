// ToneStackProcessor: Bass/Treble shelving EQ for tape echo path only.
// Realtime-safety: No allocation/locks in process()

#include "ToneFiltering.h"

#include <algorithm>

void ToneStackProcessor::Reset(float sampleRate, int numChannels)
{
    // Clear previous filters
    WDFfilters.clear();

    // Initialize WDF tone stack filters
    for (int channel = 0; channel < numChannels; channel++)
    {
        auto& filter = WDFfilters.emplace_back(std::make_unique<WDFToneStack>());
        filter->prepare(sampleRate);
        filter->updateParams(1.0, 1.0);
    }

    // Assign variables to object for DSP processing
    NumChannels = numChannels;
    smoothedBassGain.reset(sampleRate, toneSmoothingTimeSeconds);
    smoothedTrebleGain.reset(sampleRate, toneSmoothingTimeSeconds);
    smoothedBassGain.setCurrentAndTargetValue(1.0f);
    smoothedTrebleGain.setCurrentAndTargetValue(1.0f);
}

void ToneStackProcessor::clearAudioState()
{
    for (auto& filter : WDFfilters)
        if (filter)
            filter->reset();
}

void ToneStackProcessor::UpdateParameters(float Bass, float Treble, float inputlevel)
{
    // Map knob positions to gain values
    const auto bassGain = static_cast<float>(nonlinear_mapping(Bass));
    const auto trebleGain = static_cast<float>(nonlinear_mapping(Treble));
    smoothedBassGain.setTargetValue(bassGain);
    smoothedTrebleGain.setTargetValue(trebleGain);

    // Assign input level to object for DSP processing
    InputLevel = inputlevel;
}

void ToneStackProcessor::ProcessBuffer(std::vector<std::vector<float>>& buffer, int blockSize)
{
    if (blockSize <= 0)
        return;

    const auto preparedChannels = static_cast<size_t>(std::max(NumChannels, 0));
    if (preparedChannels == 0 || buffer.size() != preparedChannels || WDFfilters.size() != preparedChannels)
    {
        jassertfalse;
        return;
    }

    for (size_t channel = 0; channel < preparedChannels; ++channel)
    {
        if (!WDFfilters[channel] || buffer[channel].size() < static_cast<size_t>(blockSize))
        {
            jassertfalse;
            return;
        }
    }

    const bool smoothing = smoothedBassGain.isSmoothing() || smoothedTrebleGain.isSmoothing();
    if (smoothing)
    {
        for (int sample = 0; sample < blockSize; sample++)
        {
            const float bassGain = smoothedBassGain.getNextValue();
            const float trebleGain = smoothedTrebleGain.getNextValue();
            for (size_t channel = 0; channel < preparedChannels; ++channel)
            {
                WDFfilters[channel]->updateParams(bassGain, trebleGain);
                const auto sampleIndex = static_cast<size_t>(sample);
                buffer[channel][sampleIndex] =
                    static_cast<float>(WDFfilters[channel]->processSample(buffer[channel][sampleIndex] * InputLevel));
            }
        }

        return;
    }

    const float bassGain = smoothedBassGain.getTargetValue();
    const float trebleGain = smoothedTrebleGain.getTargetValue();
    for (size_t channel = 0; channel < preparedChannels; ++channel)
    {
        WDFfilters[channel]->updateParams(bassGain, trebleGain);
        for (int sample = 0; sample < blockSize; sample++)
        {
            const auto sampleIndex = static_cast<size_t>(sample);
            buffer[channel][sampleIndex] =
                static_cast<float>(WDFfilters[channel]->processSample(buffer[channel][sampleIndex] * InputLevel));
        }
    }
}
