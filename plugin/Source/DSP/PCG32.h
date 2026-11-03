// PCG32: small, fast pseudo-random generator with deterministic seeding.
// Realtime-safety: no allocation; deterministic for repeatable noise.
//
// seed(), nextUInt() and the default state are adapted from pcg_basic.c, the
// PCG minimal C implementation:
//   Copyright 2014 Melissa O'Neill <oneill@pcg-random.org>
//   Licensed under the Apache License, Version 2.0; see
//   docs/licenses/APACHE-2.0.txt and https://www.pcg-random.org
// Changes from the original: wrapped in a C++ class, added float output and
// random-device seeding.

#pragma once

#include <cstdint>
#include <random>

class PCG32
{
public:
    PCG32() = default;

    void seed(uint64_t initstate, uint64_t initseq)
    {
        state = 0u;
        inc = (initseq << 1u) | 1u;
        nextUInt();
        state += initstate;
        nextUInt();
    }

    void seedFromRandomDevice()
    {
        std::random_device rd;
        uint64_t s0 = (static_cast<uint64_t>(rd()) << 32u) | rd();
        uint64_t s1 = (static_cast<uint64_t>(rd()) << 32u) | rd();
        seed(s0, s1);
    }

    uint32_t nextUInt()
    {
        const uint64_t oldstate = state;
        state = oldstate * 6364136223846793005ULL + inc;
        const uint32_t xorshifted = static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
    }

    float nextFloat()
    {
        // [0, 1)
        return static_cast<float>(nextUInt()) * (1.0f / 4294967296.0f);
    }

    float nextSignedFloat()
    {
        // [-1, 1)
        return nextFloat() * 2.0f - 1.0f;
    }

private:
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc = 0xda3e39cb94b95bdbULL;
};
