// TestUtilities: Stateless signal analysis and timing utilities for all test categories.

#pragma once

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include <JuceHeader.h>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>

namespace SpatialEchoTests
{

/** The only directory a test may write to. The headless runner sets
    SPATIAL_ECHO_TEST_ARTIFACT_DIR for every run, creating a private temporary
    directory when the caller did not pass one, so parallel runs never share
    files and nothing lands in the home directory or the source tree. */
inline juce::File artifactDirectory()
{
    auto path = juce::SystemStats::getEnvironmentVariable("SPATIAL_ECHO_TEST_ARTIFACT_DIR", {}).trim();
    if (path.isEmpty())
    {
        std::error_code error;
        const auto systemTemp = std::filesystem::temp_directory_path(error);
        path = juce::String(error ? std::string("/tmp") : systemTemp.string()) + "/spatial-echo-test-artifacts";
    }
    auto directory = juce::File::getCurrentWorkingDirectory().getChildFile(path);
    directory.createDirectory();
    return directory;
}

/**
 * Common utility functions used across all test categories.
 * These are stateless helper functions for signal analysis.
 */
class TestUtilities
{
public:
    //==========================================================================
    // Signal Analysis
    //==========================================================================

    /** Find peak absolute value in a buffer */
    static float peakAbs(const float* data, int numSamples)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = std::max(peak, std::abs(data[i]));
        return peak;
    }

    /** Find peak absolute value in a vector */
    static float peakAbs(const std::vector<float>& data) { return peakAbs(data.data(), static_cast<int>(data.size())); }

    /** Calculate RMS of a buffer */
    static float rmsAbs(const float* data, int numSamples)
    {
        if (numSamples <= 0)
            return 0.0f;

        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sum += static_cast<double>(data[i]) * static_cast<double>(data[i]);

        return static_cast<float>(std::sqrt(sum / numSamples));
    }

    /** Calculate RMS of a vector */
    static float rmsAbs(const std::vector<float>& data) { return rmsAbs(data.data(), static_cast<int>(data.size())); }

    /** Convert linear amplitude to decibels */
    static float linearToDb(float linear)
    {
        if (linear <= 0.0f)
            return -100.0f;
        return 20.0f * std::log10(linear);
    }

    /** Convert decibels to linear amplitude */
    static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    //==========================================================================
    // Correlation Analysis
    //==========================================================================

    /**
     * Calculate normalized cross-correlation between two signals.
     * Returns value in range [-1, 1], where 1 = perfect match.
     */
    static float normalizedCorrelation(const float* a, const float* b, int numSamples)
    {
        if (numSamples <= 0)
            return 0.0f;

        double sumA = 0.0, sumB = 0.0, sumAB = 0.0;
        double sumA2 = 0.0, sumB2 = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            double va = static_cast<double>(a[i]);
            double vb = static_cast<double>(b[i]);
            sumA += va;
            sumB += vb;
            sumAB += va * vb;
            sumA2 += va * va;
            sumB2 += vb * vb;
        }

        double n = static_cast<double>(numSamples);
        double num = n * sumAB - sumA * sumB;
        double den = std::sqrt((n * sumA2 - sumA * sumA) * (n * sumB2 - sumB * sumB));

        if (den < 1e-10)
            return 0.0f;
        return static_cast<float>(num / den);
    }

    /** Calculate normalized correlation between two vectors */
    static float normalizedCorrelation(const std::vector<float>& a, const std::vector<float>& b)
    {
        int len = static_cast<int>(std::min(a.size(), b.size()));
        return normalizedCorrelation(a.data(), b.data(), len);
    }

    //==========================================================================
    // Tempo Sync Calculations
    //==========================================================================

    /** Note division multipliers (relative to quarter note) */
    static constexpr float noteDivisionMultipliers[] = {
        4.0f,     // 0: 1/1 (whole note)
        2.0f,     // 1: 1/2 (half note)
        1.0f,     // 2: 1/4 (quarter note)
        0.5f,     // 3: 1/8 (eighth note)
        0.25f,    // 4: 1/16 (sixteenth note)
        0.125f,   // 5: 1/32 (thirty-second note)
        0.0625f,  // 6: 1/64 (sixty-fourth note)
        // Triplets (2/3 of the straight note duration)
        1.3333f,  // 7: 1/2T (half triplet)
        0.6667f,  // 8: 1/4T (quarter triplet)
        0.3333f,  // 9: 1/8T (eighth triplet)
        0.1667f,  // 10: 1/16T (sixteenth triplet)
        0.0833f,  // 11: 1/32T (thirty-second triplet)
        // Dotted
        3.0f,    // 12: 1/2. (dotted half)
        1.5f,    // 13: 1/4. (dotted quarter)
        0.75f,   // 14: 1/8. (dotted eighth)
        0.375f,  // 15: 1/16. (dotted sixteenth)
        0.1875f  // 16: 1/32. (dotted thirty-second)
    };

    /** Calculate delay time in ms from tempo and note division index */
    static float calculateSyncedDelayMs(double bpm, int divisionIndex)
    {
        if (bpm <= 0.0)
            bpm = 120.0;
        if (divisionIndex < 0 || divisionIndex > 16)
            divisionIndex = 3;

        float quarterNoteMs = 60000.0f / static_cast<float>(bpm);
        return quarterNoteMs * noteDivisionMultipliers[divisionIndex];
    }

    //==========================================================================
    // Timing Analysis
    //==========================================================================

    /** Find sample index of first sample exceeding threshold */
    static int findOnset(const float* data, int numSamples, float threshold)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (std::abs(data[i]) > threshold)
                return i;
        }
        return -1;  // Not found
    }

    /** Find sample index of peak absolute value */
    static int findPeakIndex(const float* data, int numSamples)
    {
        int peakIdx = 0;
        float peakVal = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float val = std::abs(data[i]);
            if (val > peakVal)
            {
                peakVal = val;
                peakIdx = i;
            }
        }
        return peakIdx;
    }

    /** Convert samples to milliseconds */
    static float samplesToMs(int samples, float sampleRate)
    {
        return (static_cast<float>(samples) * 1000.0f) / sampleRate;
    }

    /** Convert milliseconds to samples */
    static int msToSamples(float ms, float sampleRate) { return static_cast<int>((ms * sampleRate) / 1000.0f); }

    //==========================================================================
    // Test Result Formatting
    //==========================================================================

    /** Format a pass/fail result with measurement */
    static juce::String formatResult(bool passed, const juce::String& testName, const juce::String& details)
    {
        juce::String result = passed ? "[PASS] " : "[FAIL] ";
        result += testName + ": " + details;
        return result;
    }

    /** Format a measurement result */
    static juce::String formatMeasurement(const juce::String& name, float value, const juce::String& unit,
                                          int decimals = 2)
    {
        return name + ": " + juce::String(value, decimals) + " " + unit;
    }

private:
    TestUtilities() = delete;  // Static-only class
};

}  // namespace SpatialEchoTests

#endif  // JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
