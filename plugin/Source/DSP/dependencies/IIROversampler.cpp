// IIROversampler: Minimum-phase polyphase IIR block oversampler (see the header).
// Realtime-safety: No allocation/locks in process() after prepare()

#include "IIROversampler.h"
#include <algorithm>

namespace echoes
{

void IIROversampler::prepare(int numChannels, int maxBlockSize, int oversampleFactor)
{
    int order = 0;
    for (int factor = std::max(1, oversampleFactor); factor > 1; factor >>= 1)
        ++order;

    if (order == 0)
    {
        oversampler.reset();
        return;
    }

    oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
        static_cast<size_t>(numChannels), static_cast<size_t>(order),
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        // isMaxQuality.
        //
        // This argument does not enable integer latency. The JUCE signature is
        //   Oversampling(numChannels, factor, type,
        //                bool isMaxQuality = true,
        //                bool useIntegerLatency = false)
        // so this positional `true` is isMaxQuality and integer latency is OFF,
        // leaving a fractional group delay (4.433 samples at 4x) that no caller
        // compensates. Passing a 5th `true` would round it to an integer, but it
        // does so by inserting a fractional-delay line into the signal path, so
        // that would be a deliberate audio change.
        true);
    oversampler->initProcessing(static_cast<size_t>(std::max(1, maxBlockSize)));
}

void IIROversampler::reset()
{
    if (oversampler)
        oversampler->reset();
}

juce::dsp::AudioBlock<float> IIROversampler::processSamplesUp(const juce::dsp::AudioBlock<float>& inputBlock)
{
    if (!oversampler)
    {
        oversampledNumSamples = static_cast<int>(inputBlock.getNumSamples());
        return inputBlock;
    }

    auto oversampledBlock = oversampler->processSamplesUp(inputBlock);
    oversampledNumSamples = static_cast<int>(oversampledBlock.getNumSamples());
    return oversampledBlock;
}

void IIROversampler::processSamplesDown(juce::dsp::AudioBlock<float>& outputBlock)
{
    if (oversampler)
        oversampler->processSamplesDown(outputBlock);
}

}  // namespace echoes
