// BinauralSelfTests (declarations): Self-tests that need BinauralProcessor internals.

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

// BinauralProcessor declares this struct a friend, so these tests can drive
// its private convolution engine, SLERP helper and convolvers directly.
struct BinauralProcessorTestAccess
{
    // Exact-parity self-test of ConvolutionEngine vs direct time-domain
    // convolution (arbitrary callbacks, mid-hop IR changes, reset/in-place).
    static juce::String runConvolutionParitySelfTest();
    // Measures the group delay of each path through the binaural stage (dry,
    // low band, high band) from a captured impulse response, for both the
    // aligned and arbitrary callbacks, and gates the crossover response and
    // uncompensated callback/wet-dry parity.
    static juce::String runBinauralLatencySelfTest();
    // Analytic ramp/trajectory references for bypass and opposite directions.
    static juce::String runControlContinuitySelfTest();
};

#endif
