// InterpolatedDelayLine: Delay line with Lagrange interpolation for fractional delays in allpass filters.
// Realtime-safety: No allocation/locks in setDelay() or process(); supports fractional sample delays.
// Based on: Välimäki, Parker, Abel spring reverb papers

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <algorithm>

/**
 * Delay line with Lagrange interpolation for fractional delays
 */
class InterpolatedDelayLine
{
public:
    InterpolatedDelayLine(int maxDelaySamples = 65536)
        : maxDelay(maxDelaySamples)
    {
        buffer.resize(static_cast<size_t>(maxDelay), 0.0f);
        bufferSize = static_cast<int>(buffer.size());
        maxDelayForInterp = bufferSize - 4;
        updateWrapState();
    }

    void prepare(float sampleRate, int maxSamples)
    {
        fs = sampleRate;
        buffer.resize(static_cast<size_t>(std::max(maxSamples, maxDelay)), 0.0f);
        bufferSize = static_cast<int>(buffer.size());
        maxDelayForInterp = bufferSize - 4;
        updateWrapState();
        reset();
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        allpassInterpZ1 = 0.0f;
    }

    void setDelay(float delaySamples)
    {
        delayInSamples = std::max(1.0f, std::min(delaySamples, static_cast<float>(maxDelayForInterp)));
    }

    float getDelay() const { return delayInSamples; }
    int getMaxDelayForInterp() const { return maxDelayForInterp; }
    bool hasPowerOfTwoBuffer() const noexcept { return isPowerOfTwo; }

    void pushSample(float sample)
    {
        buffer[static_cast<size_t>(writeIndex)] = sample;
        if (isPowerOfTwo)
        {
            writeIndex = (writeIndex + 1) & bufferMask;
        }
        else
        {
            ++writeIndex;
            if (writeIndex >= bufferSize)
                writeIndex = 0;
        }
    }

    void pushSampleNoWrapCheck(float sample)
    {
        // Caller guarantees power-of-two buffer size.
        buffer[static_cast<size_t>(writeIndex)] = sample;
        writeIndex = (writeIndex + 1) & bufferMask;
    }

    float popSample() { return popSampleAt(delayInSamples); }

    /**
     * Read sample at a specific delay (for LFO modulation)
     * @param delay Delay in samples (can be fractional)
     */
    float popSampleAt(float delay)
    {
        // Clamp delay to valid range (need 4 samples for cubic interpolation)
        return popSampleAtNoClamp(std::max(4.0f, std::min(delay, static_cast<float>(maxDelayForInterp))));
    }

    float popSampleAtNoClamp(float delay)
    {
        // Caller guarantees delay within [4, maxDelayForInterp].
        const float readPos = static_cast<float>(writeIndex) - delay;

        int idx0 = 0;
        float frac = 0.0f;
        int idx1 = 0;
        int idx2 = 0;
        int idxm1 = 0;

        if (isPowerOfTwo)
        {
            const int base = fastFloorToInt(readPos);
            const unsigned int mask = static_cast<unsigned int>(bufferMask);
            const unsigned int idx0u = static_cast<unsigned int>(base) & mask;
            idx0 = static_cast<int>(idx0u);
            frac = readPos - static_cast<float>(base);

            idx1 = static_cast<int>((idx0u + 1u) & mask);
            idx2 = static_cast<int>((idx0u + 2u) & mask);
            idxm1 = static_cast<int>((idx0u - 1u) & mask);
        }
        else
        {
            // Floor readPos and wrap in INTEGER arithmetic.
            //
            // Wrapping a float readPos by adding bufferSize can produce
            // idx0 == bufferSize, a read one element past the end of the
            // buffer. The spring tank's pre-delay line is the only
            // non-power-of-two InterpolatedDelayLine in the plugin
            // (ceil(fs * 0.2) + 8 samples, 9608 at 48 kHz), and on the exact
            // sample where writeIndex catches up with the requested delay it
            // produces readPos = -2^-16. The float ULP at 9608 is 2^-10,
            // roughly 64x larger, so -0.0000153 + 9608 rounds to exactly 9608.
            // That read returns whatever the vector's spare capacity holds
            // (values down to -1.3e+23 measured) and can ring the tank up to
            // its output clipper.
            //
            // Flooring first also keeps frac exact: adding bufferSize to
            // readPos discards the low fractional bits (frac 0.6 comes back as
            // 0.5996 at bufferSize 9608).
            const int base = fastFloorToInt(readPos);
            frac = readPos - static_cast<float>(base);

            idx0 = wrapIndexAny(base);
            idx1 = wrapIndex(idx0 + 1);
            idx2 = wrapIndex(idx1 + 1);
            idxm1 = wrapIndex(idx0 - 1);
        }

        const float* data = buffer.data();
        float ym1 = data[idxm1];
        float y0 = data[idx0];
        float y1 = data[idx1];
        float y2 = data[idx2];

        return cubicLagrange(ym1, y0, y1, y2, frac);
    }

    float popSampleAtAllpass(float delay)
    {
        delay = std::max(kMinAllpassDelay, std::min(delay, static_cast<float>(maxDelayForInterp) - 1.0f));
        return allpassRead(delay);
    }

    float popSampleAtNoClampAllpass(float delay)
    {
        // Caller guarantees delay within [kMinAllpassDelay, maxDelayForInterp - 1].
        return allpassRead(delay);
    }

    // Minimum delay the first-order-allpass reader can service. The reader
    // biases the fractional part into [0.5, 1.5) (see allpassRead), so it needs
    // one whole sample of slack below the requested delay.
    static constexpr float kMinAllpassDelay = 2.0f;

    float processSample(float input)
    {
        float output = popSample();
        pushSample(input);
        return output;
    }

public:
    // 4-point, 3rd-order Lagrange interpolation between y0 (at frac 0) and
    // y1 (at frac 1), with ym1/y2 as the outer support points.
    //
    // Derived directly from the Lagrange basis over x = {-1, 0, 1, 2}:
    //   c3 = (y2 - ym1)/6 + (y0 - y1)/2
    // The similar-looking
    //   c3 = (y1 - ym1)/6 + (y0 - y1)/2 + (y2 - y0)/6
    // differs by +(y1 - y0)/6: the polynomial then misses its own support
    // point (p(1) = y1 + (y1 - y0)/6 instead of y1), the 4-tap kernel peaks
    // at 1.33 (+2.5 dB) as frac approaches 1, and a modulated delay steps
    // every time it crosses an integer sample boundary. Verified against the
    // Lagrange basis and against Utils::HermiteInterp's endpoint behaviour.
    static inline float cubicLagrange(float ym1, float y0, float y1, float y2, float frac) noexcept
    {
        const float c0 = y0;
        const float c1 = y1 - ym1 * (1.0f / 3.0f) - y0 * 0.5f - y2 * (1.0f / 6.0f);
        const float c2 = (y1 + ym1) * 0.5f - y0;
        const float c3 = (y2 - ym1) * (1.0f / 6.0f) + (y0 - y1) * 0.5f;
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

private:
    // First-order-allpass fractional delay read.
    //
    // For a requested delay D the reader splits D = n + d with the fractional
    // part biased into [0.5, 1.5), reads the integer taps at delay n and n+1,
    // and runs the allpass y[k] = eta*A + B - eta*y[k-1] with
    // eta = (1 - d)/(1 + d), whose DC group delay is exactly n + d = D.
    //
    // The simpler form  y = B + frac*(A - y[k-1]), i.e. eta = frac with A/B
    // swapped, realises a delay of n + (1 - frac)/(1 + frac): the delay runs
    // *backwards* inside every integer bin and jumps by two whole samples at
    // each bin boundary (measured: D = 8.999 -> 8.074, D = 9.000 -> 10.000).
    // On a modulated line that is a periodic click, not a smooth sweep.
    //
    // Biasing d into [0.5, 1.5) also keeps the allpass pole at |-eta| <= 1/3,
    // so the filter settles quickly instead of ringing near Nyquist when the
    // fractional part approaches zero.
    inline float allpassRead(float delay) noexcept
    {
        int n = fastFloorToInt(delay);
        float d = delay - static_cast<float>(n);
        if (d < 0.5f)
        {
            n -= 1;
            d += 1.0f;
        }

        // writeIndex and n are both integers, so the tap index is derived in
        // integer arithmetic. Routing it through a float and adding bufferSize
        // to a small negative value rounds up to bufferSize itself and reads
        // one element past the end of the buffer - see the note in
        // popSampleAtNoClamp. On power-of-two buffers wrapIndexAny is a plain
        // mask.
        const int idxA = wrapIndexAny(writeIndex - n);
        const int idxB = wrapIndex(idxA - 1);

        const float* data = buffer.data();
        const float eta = (1.0f - d) / (1.0f + d);
        allpassInterpZ1 = eta * data[idxA] + data[idxB] - eta * allpassInterpZ1;
        return allpassInterpZ1;
    }

    static inline int fastFloorToInt(float x) noexcept
    {
        const int i = static_cast<int>(x);
        return (x < 0.0f && !juce::exactlyEqual(x, static_cast<float>(i))) ? (i - 1) : i;
    }

    inline void updateWrapState()
    {
        isPowerOfTwo = (bufferSize > 0) && ((bufferSize & (bufferSize - 1)) == 0);
        bufferMask = isPowerOfTwo ? (bufferSize - 1) : 0;
    }

    inline int wrapIndex(int idx) const noexcept
    {
        if (isPowerOfTwo)
            return idx & bufferMask;
        if (idx < 0)
            return idx + bufferSize;
        if (idx >= bufferSize)
            return idx - bufferSize;
        return idx;
    }

    // Wrap an index that may be any distance outside [0, bufferSize).
    // wrapIndex above corrects by at most one buffer length, which is enough
    // for the +-1 neighbours of an already-wrapped tap but not for a raw
    // writeIndex - delay difference. Only the non-power-of-two path pays for
    // the divide, and the only non-power-of-two line in the plugin is the
    // spring pre-delay, read once per sample.
    inline int wrapIndexAny(int idx) const noexcept
    {
        if (isPowerOfTwo)
            return static_cast<int>(static_cast<unsigned int>(idx) & static_cast<unsigned int>(bufferMask));
        idx %= bufferSize;
        return idx < 0 ? idx + bufferSize : idx;
    }

    std::vector<float> buffer;
    int maxDelay;
    int writeIndex = 0;
    float delayInSamples = 1.0f;
    float fs = 44100.0f;
    int bufferSize = 0;
    int maxDelayForInterp = 0;
    int bufferMask = 0;
    bool isPowerOfTwo = false;
    float allpassInterpZ1 = 0.0f;
};
