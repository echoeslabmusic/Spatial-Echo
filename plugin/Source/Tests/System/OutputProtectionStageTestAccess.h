// OutputProtectionStageTestAccess: Test-only access to the output protection stage's measured oversampler latency.

#pragma once

#include <JuceHeader.h>

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include "../../DSP/OutputProtectionStage.h"

struct OutputProtectionStageTestAccess
{
    // Latency the prepared oversampler reports, which must equal the fixed
    // OutputProtectionStage::latencySamples the host is told about.
    static float measuredLatency(const OutputProtectionStage& stage) noexcept
    {
        return stage.channels.empty() ? 0.0f : stage.channels.front().oversampler.getLatencyInSamples();
    }
};

#endif
