// CircularBuffer: Fixed-size delay buffer with configurable interpolation for time-varying delay reads.
// Realtime-safety: No allocation/locks in process() after Init()

#include "CircularBuffer.h"

CircularBuffer::CircularBuffer() = default;
CircularBuffer::~CircularBuffer() = default;

// Function to reset all buffer values to zero
void CircularBuffer::ClearBuffer()
{
    if (delaybuffer == nullptr)
        return;

    // iterate through buffer
    for (unsigned int i = 0; i < buffer_length; i++)
    {
        // set to zero
        delaybuffer[i] = 0.0f;
    }
}

// Function to initialise the buffer using the sample rate and the maximum desired delay time.
void CircularBuffer::Init(int sr, float max_delay_time_ms)
{
    // Save variables into object
    SampleRate = static_cast<float>(sr);
    MaxDelayTimeMs = max_delay_time_ms;

    // Calculate buffer length
    const double requestedSamples = ((double)MaxDelayTimeMs / 1000.0) * (double)SampleRate + 1.0;
    unsigned int requested = (requestedSamples > 1.0) ? (unsigned int)requestedSamples : 1u;

    // Round up to the next power of two with integer arithmetic. A
    // pow(2, ceil(log(n)/log(2))) form is exact only when the double log lands
    // on the right side of an integer, and gives a zero-length buffer for n == 0.
    unsigned int rounded = 1u;
    while (rounded < requested && rounded < 0x40000000u)
        rounded <<= 1u;
    buffer_length = rounded;

    // Reset array with new buffer length
    delaybuffer.reset(new float[buffer_length]);

    // Initialise array values to zero
    ClearBuffer();

    // Initialise buffer write index
    WriteIndex = 0;
}

// Function that reads from the buffer with a desired amount of delay in ms. Calculates delay in samples using sample
// rate. Optional higher-order interpolation for fractional delay.
float CircularBuffer::BufferRead(float delay_time_ms, bool interpolate_line)
{
    if (delaybuffer == nullptr || buffer_length == 0)
        return 0.0f;

    // Clamp delay time to minimum 0.5ms to prevent negative/zero delay issues
    // This avoids interpolation artifacts when wow/flutter modulation causes negative values
    if (!std::isfinite(delay_time_ms) || delay_time_ms < 0.5f)
        delay_time_ms = 0.5f;

    // Integer + fractional delay in samples. Computed in double: a float
    // product loses fractional resolution once the delay passes ~2^19 samples
    // (0.125 sample at 6 s / 192 kHz), which quantizes the wow/flutter
    // modulation into visible steps on long tempo-synced delays.
    const double delaySamples = ((double)delay_time_ms / 1000.0) * (double)SampleRate;
    const int wholeSamples = (int)delaySamples;
    float t = (float)(delaySamples - (double)wholeSamples);
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;

    // WriteIndex points at the latest written sample because BufferWrite()
    // advances before storing. A 1-sample delay therefore reads WriteIndex.
    // Signed arithmetic throughout, so wrapIndex() never sees an unsigned
    // value narrowed to int.
    int ReadIndex = wrapIndex((int)WriteIndex + 1 - wholeSamples);

    // Calculate normal output
    float yn = sampleAt(ReadIndex);

    // interpolation enabled check
    if (interpolate_line)
    {
        // 6-point Lagrange interpolation centered around current read position.
        // This improves fractional-delay accuracy for modulated reads compared
        // to 4-point cubic, while keeping CPU cost reasonable.
        const int i_m2 = wrapIndex(ReadIndex + 2);
        const int i_m1 = wrapIndex(ReadIndex + 1);
        const int i0 = ReadIndex;
        const int i1 = wrapIndex(ReadIndex - 1);
        const int i2 = wrapIndex(ReadIndex - 2);
        const int i3 = wrapIndex(ReadIndex - 3);

        const float y_m2 = sampleAt(i_m2);
        const float y_m1 = sampleAt(i_m1);
        const float y0 = sampleAt(i0);
        const float y1 = sampleAt(i1);
        const float y2 = sampleAt(i2);
        const float y3 = sampleAt(i3);

        const float lagrangeOut = lagrange6Interp(y_m2, y_m1, y0, y1, y2, y3, t);
        if (std::isfinite(lagrangeOut))
            yn = lagrangeOut;
        else
            yn = cubicInterp(y_m1, y0, y1, y2, t);
    }

    // return output
    return yn;
}

// Function to read from the buffer if delay required is explicitly in samples. Does not interpolate.
float CircularBuffer::BufferReadSamples(int delay_in_samples)
{
    if (delaybuffer == nullptr || buffer_length == 0)
        return 0.0f;

    // WriteIndex points at the latest written sample because BufferWrite()
    // advances before storing. A 1-sample delay therefore reads WriteIndex.
    return sampleAt(wrapIndex((int)WriteIndex + 1 - delay_in_samples));
}

// Function to write to buffer
void CircularBuffer::BufferWrite(float xn)
{
    if (delaybuffer == nullptr || buffer_length == 0)
        return;

    // Increment write index
    WriteIndex++;

    // Wrap write index with modulo
    WriteIndex %= buffer_length;

    // Write sample to buffer
    delaybuffer[WriteIndex] = xn;
}
