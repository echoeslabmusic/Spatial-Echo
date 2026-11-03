// ScopeDataFifo: Lock-free single-producer single-consumer FIFO for oscilloscope display data.
// Realtime-safety: No locks, safe in process() and UI thread

#pragma once

#include <JuceHeader.h>
#include <array>

// Lock-free SPSC FIFO for passing audio data from the audio thread to the UI.
class ScopeDataFifo
{
public:
    static constexpr int kCapacity = 16384;

    ScopeDataFifo()
        : abstractFifo(kCapacity)
    {
        data.fill(0.0f);
    }

    void push(const float* samples, int numSamples) noexcept
    {
        int toWrite = juce::jmin(numSamples, abstractFifo.getFreeSpace());
        if (toWrite == 0)
            return;
        auto scope = abstractFifo.write(toWrite);
        for (int i = 0; i < scope.blockSize1; ++i)
            data[static_cast<size_t>(scope.startIndex1 + i)] = samples[i];
        for (int i = 0; i < scope.blockSize2; ++i)
            data[static_cast<size_t>(scope.startIndex2 + i)] = samples[scope.blockSize1 + i];
    }

    int pull(float* dest, int maxSamples) noexcept
    {
        int avail = juce::jmin(maxSamples, abstractFifo.getNumReady());
        if (avail == 0)
            return 0;
        auto scope = abstractFifo.read(avail);
        for (int i = 0; i < scope.blockSize1; ++i)
            dest[i] = data[static_cast<size_t>(scope.startIndex1 + i)];
        for (int i = 0; i < scope.blockSize2; ++i)
            dest[scope.blockSize1 + i] = data[static_cast<size_t>(scope.startIndex2 + i)];
        return avail;
    }

private:
    juce::AbstractFifo abstractFifo;
    std::array<float, kCapacity> data;
};
