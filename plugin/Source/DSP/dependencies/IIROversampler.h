// IIROversampler: Block oversampler for the preamp saturation stage, built on JUCE's minimum-phase polyphase IIR
//   half-band filters (low latency, preserves transients).
// Realtime-safety: No allocation in processSamplesUp()/processSamplesDown(); prepare() allocates.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <memory>

namespace echoes
{

/**
 * Usage:
 *   IIROversampler oversampler;
 *   oversampler.prepare(numChannels, maxBlockSize, oversampleFactor);
 *
 *   // In process loop:
 *   auto oversampledBlock = oversampler.processSamplesUp(inputBlock);
 *   // ... process at oversampled rate ...
 *   oversampler.processSamplesDown(inputBlock);
 */
class IIROversampler
{
public:
    /**
     * Prepare the oversampler for processing. Allocates.
     * @param numChannels Number of audio channels
     * @param maxBlockSize Maximum expected block size at base sample rate
     * @param oversampleFactor Power of 2 factor (1, 2, 4, 8); 1 passes blocks through unchanged
     */
    void prepare(int numChannels, int maxBlockSize, int oversampleFactor);

    /**
     * Reset the filter histories.
     */
    void reset();

    /**
     * Upsample the input block
     * @param inputBlock Audio block at base sample rate
     * @return Block at the oversampled rate (the input block itself at factor 1)
     */
    juce::dsp::AudioBlock<float> processSamplesUp(const juce::dsp::AudioBlock<float>& inputBlock);

    /**
     * Downsample back to original rate
     * @param outputBlock Audio block to write downsampled result
     */
    void processSamplesDown(juce::dsp::AudioBlock<float>& outputBlock);

    /**
     * Get the number of samples in oversampled buffer after last processSamplesUp
     */
    int getOversampledNumSamples() const noexcept { return oversampledNumSamples; }

private:
    // Null at factor 1.
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int oversampledNumSamples = 0;
};

}  // namespace echoes
