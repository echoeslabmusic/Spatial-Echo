// CircularBuffer: Fixed-size delay buffer with configurable interpolation for time-varying delay reads.
// Realtime-safety: No allocation/locks in process() after Init()

#pragma once

#include <cmath>
#include <memory>

class CircularBuffer
{
public:
    // C-tor
    CircularBuffer();
    // D-tor
    ~CircularBuffer();

    // Public member functions
    void Init(int sr, float max_delay_time_ms);
    void ClearBuffer();
    void BufferWrite(float xn);
    float BufferRead(float delay_time_ms, bool interpolate_line);
    float BufferReadSamples(int delay_in_samples);

private:
    // Private member variables
    unsigned int WriteIndex = 0;
    float SampleRate = 0.0f;
    unsigned int buffer_length = 0;

    float MaxDelayTimeMs = 0.0f;

    // Unique ptr array to use as circular buffer, self deletes when goes out of scope.
    std::unique_ptr<float[]> delaybuffer = nullptr;

    float sampleAt(int index) const { return delaybuffer[static_cast<size_t>(index)]; }

    int wrapIndex(int index) const
    {
        if (buffer_length == 0)
            return 0;
        int len = static_cast<int>(buffer_length);
        index %= len;
        if (index < 0)
            index += len;
        return index;
    }

    float cubicInterp(float y0, float y1, float y2, float y3, float t)
    {
        float a0 = (-0.5f * y0) + (1.5f * y1) - (1.5f * y2) + (0.5f * y3);
        float a1 = y0 - (2.5f * y1) + (2.0f * y2) - (0.5f * y3);
        float a2 = (-0.5f * y0) + (0.5f * y2);
        float a3 = y1;
        return ((a0 * t + a1) * t + a2) * t + a3;
    }

    // 6-point Lagrange interpolation (x positions: -2, -1, 0, 1, 2, 3)
    float lagrange6Interp(float y_m2, float y_m1, float y0, float y1, float y2, float y3, float t)
    {
        float tm2 = t + 2.0f;
        float tm1 = t + 1.0f;
        float t0 = t;
        float t1 = t - 1.0f;
        float t2 = t - 2.0f;
        float t3 = t - 3.0f;

        float c0 = (tm1 * t0 * t1 * t2 * t3) / -120.0f;  // x = -2
        float c1 = (tm2 * t0 * t1 * t2 * t3) / 24.0f;    // x = -1
        float c2 = (tm2 * tm1 * t1 * t2 * t3) / -12.0f;  // x = 0
        float c3 = (tm2 * tm1 * t0 * t2 * t3) / 12.0f;   // x = 1
        float c4 = (tm2 * tm1 * t0 * t1 * t3) / -24.0f;  // x = 2
        float c5 = (tm2 * tm1 * t0 * t1 * t2) / 120.0f;  // x = 3

        return (y_m2 * c0) + (y_m1 * c1) + (y0 * c2) + (y1 * c3) + (y2 * c4) + (y3 * c5);
    }
};
