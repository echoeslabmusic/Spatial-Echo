/*
 * PerSampleOversampler.h
 *
 * Per-sample oversampling (2x/4x/8x) for nonlinear feedback-loop processing.
 * Implemented with JUCE polyphase IIR oversampling to avoid external
 * dependencies while preserving low-latency behavior.
 */

// PerSampleOversampler: Template-based IIR oversampler (2x/4x/8x) for nonlinear feedback and dynamic processes.
// Realtime-safety: No allocation/locks in process() after prepare()

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <memory>

namespace echoes
{

template <int Order>
class PerSampleOversampler
{
    static_assert(Order >= 1 && Order <= 3, "Order must be 1 (2x), 2 (4x), or 3 (8x)");

public:
    PerSampleOversampler() { initialize(); }

    // Copying is deleted on purpose: a copy would have to call initialize(),
    // which allocates three times (make_unique, AudioBuffer::setSize,
    // Oversampling::initProcessing), so copying one anywhere near the audio
    // thread would allocate. Owners default-construct in place
    // (TapeRecordSaturator and OutputProtectionStage clear() then resize()
    // their vectors). Moves stay available and noexcept so vector growth still
    // works without copying.
    PerSampleOversampler(const PerSampleOversampler&) = delete;
    PerSampleOversampler& operator=(const PerSampleOversampler&) = delete;

    PerSampleOversampler(PerSampleOversampler&&) noexcept = default;
    PerSampleOversampler& operator=(PerSampleOversampler&&) noexcept = default;

    // The factor is the template factor at every host rate. Measured (single
    // sine, bin-exact 2^18 FFT, alias energy in 20 Hz - 20 kHz relative to the
    // fundamental, 12 kHz tone at amplitude 2.0, the worst case in the sweep):
    //
    //   output-protection soft-knee limiter   1x        2x
    //     fs =  48 kHz                     -12.66    -30.73
    //     fs =  96 kHz                     -30.73    -41.18
    //     fs = 192 kHz                     -41.18    -55.20
    //
    //   ImprovedSpringReverb tanh             1x        2x
    //     fs =  48 kHz                     -15.22    -40.64
    //     fs =  96 kHz                     -40.64    -90.77
    //     fs = 192 kHz                     -90.77   -154.15
    //
    // The diagonals are exact: 96 kHz at 1x measures the same alias level as
    // 48 kHz at 2x, because the nonlinearity sees the same rate either way, so
    // running the full factor at high host rates only lowers aliasing further.
    // TapeRecordSaturator applies its own rate rule (8x at <= 48 kHz, 4x
    // above) by choosing between two instances.
    //
    // The half-band IIR filters are defined relative to the host rate, so
    // preparing needs no sample rate. Allocates when integerLatency changes;
    // prepare() is not called on the audio thread.
    void prepare(bool integerLatency = false)
    {
        if (integerLatency != useIntegerLatency)
        {
            useIntegerLatency = integerLatency;
            rebuild();
        }

        reset();
    }

    /** Round-trip latency in samples at the BASE rate.
     *
     *  Internal feedback stages retain fractional latency. Measured values for
     *  filterHalfBandPolyphaseIIR at maximum quality: 3.137 samples for 2x,
     *  4.433 for 4x, 4.951 for 8x. The figure is fractional because this is a
     *  minimum-phase IIR design. The final output stage opts into JUCE's
     *  integer-latency correction; frequency-dependent phase still remains.
     */
    float getLatencyInSamples() const { return oversampler != nullptr ? oversampler->getLatencyInSamples() : 0.0f; }

    void reset()
    {
        if (oversampler != nullptr)
            oversampler->reset();

        inputBuffer.clear();
        outputBuffer.clear();
    }

    template <typename Processor>
    float process(float input, Processor&& processor)
    {
        // A moved-from instance has no oversampler: run the nonlinearity at the
        // host rate.
        if (oversampler == nullptr)
            return processor(input);

        inputBuffer.setSample(0, 0, input);

        juce::dsp::AudioBlock<float> inBlock(inputBuffer);
        auto osBlock = oversampler->processSamplesUp(inBlock);

        const int osSamples = static_cast<int>(osBlock.getNumSamples());
        for (int i = 0; i < osSamples; ++i)
        {
            const float x = osBlock.getSample(0, i);
            osBlock.setSample(0, i, processor(x));
        }

        juce::dsp::AudioBlock<float> outBlock(outputBuffer);
        oversampler->processSamplesDown(outBlock);
        return outputBuffer.getSample(0, 0);
    }

private:
    void initialize() { rebuild(); }

    // Allocates. Only ever called from the constructor and from prepare().
    void rebuild()
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
            1, static_cast<size_t>(Order), juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true,
            useIntegerLatency);
        oversampler->initProcessing(1);

        inputBuffer.setSize(1, 1, false, false, true);
        outputBuffer.setSize(1, 1, false, false, true);
        inputBuffer.clear();
        outputBuffer.clear();
        oversampler->reset();
    }

    bool useIntegerLatency = false;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> inputBuffer;
    juce::AudioBuffer<float> outputBuffer;
};

class PerSampleOversampler2x : public PerSampleOversampler<1>
{
};
class PerSampleOversampler4x : public PerSampleOversampler<2>
{
};
class PerSampleOversampler8x : public PerSampleOversampler<3>
{
};

}  // namespace echoes
