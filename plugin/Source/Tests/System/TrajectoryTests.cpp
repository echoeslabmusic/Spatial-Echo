/*
  ==============================================================================

    TrajectoryTests.cpp
    Trajectory pattern shape tests: validate evaluateTrajectory() produces
    correct waveform characteristics for each spatial motion pattern.

  ==============================================================================
*/

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS

#include <JuceHeader.h>
#include "SystemTests.h"
#include "../../DSP/SpatialMotionTrajectory.h"
#include "../../DSP/EchoesModel.h"  // calculateTapPhaseOffset - the real choreography contract
#include "../../SpatialParamIDs.h"
#include <cmath>
#include <algorithm>

namespace SpatialEchoTests::System
{

//==============================================================================
// Orbit Azimuth Continuity Test
// Verifies azimuth wraps seamlessly across ±180° boundary
//==============================================================================

juce::String runOrbitAzimuthContinuityTest()
{
    // Comparing |beforeAz| against |afterAz| at a single phase boundary is not
    // enough: any function whose magnitude is stable across the seam satisfies
    // it, including a constant +180 offset or an orbit that never moves.
    //
    // The contract has two halves, and both are asserted here:
    //   1. Continuity: the shortest-arc step between adjacent phase samples is
    //      bounded everywhere, INCLUDING across the +/-180 seam. Measuring the
    //      step with std::remainder(delta, 360) is what makes the seam a
    //      non-event; a genuine discontinuity shows up as an oversized step.
    //   2. Travel: the accumulated shortest-arc steps sum to a full 360 deg
    //      per cycle. Without this a frozen or half-range azimuth passes (1).
    constexpr float depth = 1.0f;
    constexpr int steps = 720;  // 0.5 deg of ideal travel per step
    constexpr double phaseStep = 1.0 / static_cast<double>(steps);
    const float idealStepDeg = 360.0f / static_cast<float>(steps);
    // 4x the ideal step catches a real jump (the seam would show ~360) while
    // tolerating float rounding on the phase accumulator.
    const float maxAllowedStepDeg = idealStepDeg * 4.0f;

    float previousAz =
        spatialmotion::evaluateTrajectory(SpatialIDs::motion::Pattern::Orbit, -phaseStep, depth).azimuthDeg;

    float worstStepDeg = 0.0f;
    double worstStepPhase = 0.0;
    double accumulatedTravelDeg = 0.0;

    for (int i = 0; i < steps; ++i)
    {
        const double phase = static_cast<double>(i) * phaseStep;
        const float az = spatialmotion::evaluateTrajectory(SpatialIDs::motion::Pattern::Orbit, phase, depth).azimuthDeg;

        // Shortest-arc step: +179.5 -> -179.5 is a 1 deg move, not a 359 deg one.
        const float stepDeg = std::remainder(az - previousAz, 360.0f);
        accumulatedTravelDeg += stepDeg;

        if (std::abs(stepDeg) > worstStepDeg)
        {
            worstStepDeg = std::abs(stepDeg);
            worstStepPhase = phase;
        }

        previousAz = az;
    }

    const bool continuous = worstStepDeg <= maxAllowedStepDeg;
    const bool travelsFullCircle = std::abs(std::abs(accumulatedTravelDeg) - 360.0) < 1.0;
    const bool passed = continuous && travelsFullCircle;

    juce::String result;
    result << "OrbitAzimuthContinuity: worstStep=" << juce::String(worstStepDeg, 3)
           << "deg @phase=" << juce::String(worstStepPhase, 4) << " (limit=" << juce::String(maxAllowedStepDeg, 3)
           << "deg)"
           << " travel=" << juce::String(accumulatedTravelDeg, 2) << "deg"
           << " continuous=" << (continuous ? "yes" : "NO") << " fullCircle=" << (travelsFullCircle ? "yes" : "NO")
           << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Figure8 Elevation-to-Azimuth Frequency Ratio Test
// Verifies elevation modulates at 2x the azimuth frequency (lissajous)
//==============================================================================

juce::String runFigure8FrequencyRatioTest()
{
    constexpr float depth = 1.0f;

    // Count zero crossings over one cycle. Seed the previous sample from just
    // before phase 0 so crossings at the cycle boundary count - a half-open
    // interval seeded with 0 undercounts az (1 vs el 3 reads as ratio 3).
    int azZeroCrossings = 0;
    int elZeroCrossings = 0;
    const auto seed = spatialmotion::evaluateTrajectory(SpatialIDs::motion::Pattern::Figure8, -0.01, depth);
    float lastAz = seed.azimuthDeg;
    float lastEl = seed.elevationDeg;

    for (double phase = 0.0; phase < 1.0; phase += 0.01)
    {
        const auto offset = spatialmotion::evaluateTrajectory(SpatialIDs::motion::Pattern::Figure8, phase, depth);

        // Count zero crossings
        if ((lastAz < 0.0f && offset.azimuthDeg >= 0.0f) || (lastAz >= 0.0f && offset.azimuthDeg < 0.0f))
            ++azZeroCrossings;

        if ((lastEl < 0.0f && offset.elevationDeg >= 0.0f) || (lastEl >= 0.0f && offset.elevationDeg < 0.0f))
            ++elZeroCrossings;

        lastAz = offset.azimuthDeg;
        lastEl = offset.elevationDeg;
    }

    // Elevation should cross zero ~2x for each azimuth crossing
    const float ratio =
        (azZeroCrossings > 0) ? static_cast<float>(elZeroCrossings) / static_cast<float>(azZeroCrossings) : 0.0f;
    const bool ratioCorrect = std::abs(ratio - 2.0f) < 0.5f;  // Allow ±0.5x tolerance
    const bool passed = ratioCorrect;

    juce::String result;
    result << "Figure8FrequencyRatio: azZeros=" << azZeroCrossings << " elZeros=" << elZeroCrossings
           << " ratio=" << juce::String(ratio, 2) << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Breathe Distance-Only Modulation Test
// Verifies only distance is modulated, azimuth and elevation are zero
//==============================================================================

juce::String runBreatheDistanceOnlyTest()
{
    constexpr float depth = 1.0f;
    bool azElClean = true;
    float maxAbsDist = 0.0f;

    // The distance sine legitimately crosses zero (phases 0 and 0.5), so
    // "distance varies" is asserted on the cycle's peak, not per sample.
    for (double phase = 0.0; phase < 1.0; phase += 0.05)
    {
        const auto offset = spatialmotion::evaluateTrajectory(SpatialIDs::motion::Pattern::Breathe, phase, depth);

        if (std::abs(offset.azimuthDeg) > 0.001f || std::abs(offset.elevationDeg) > 0.001f)
            azElClean = false;

        maxAbsDist = std::max(maxAbsDist, std::abs(offset.distanceMeters));
    }

    const bool distanceModulates = maxAbsDist > 1.0f;  // +/-1.5 m at full depth
    const bool passed = azElClean && distanceModulates;

    juce::String result;
    result << "BreatheDistanceOnly: azElClean=" << (azElClean ? "yes" : "no")
           << " maxAbsDist=" << juce::String(maxAbsDist, 2) << "m " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Depth=0 Zero Offsets Test
// Verifies all patterns return exactly zero offsets when depth=0
//==============================================================================

juce::String runDepthZeroStillnessTest()
{
    constexpr float depth = 0.0f;
    const int patterns[] = {0, 1, 2, 3, 4, 5};  // Still through Wander
    bool failed = false;

    for (int patternIdx : patterns)
    {
        const auto pattern = static_cast<SpatialIDs::motion::Pattern>(patternIdx);

        for (double phase = 0.0; phase < 1.0; phase += 0.25)
        {
            const auto offset = spatialmotion::evaluateTrajectory(pattern, phase, depth);

            if (std::abs(offset.azimuthDeg) > 1e-6f || std::abs(offset.elevationDeg) > 1e-6f ||
                std::abs(offset.distanceMeters) > 1e-6f)
            {
                failed = true;
                break;
            }
        }
        if (failed)
            break;
    }

    const bool passed = !failed;
    juce::String result;
    result << "DepthZeroStillness: " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

//==============================================================================
// Choreography Phase Offsets Test
// Verifies Constellation and Cascade produce different tap phases
//==============================================================================

juce::String runChoreographyOffsetsTest()
{
    // Exercises the REAL DSP function (calculateTapPhaseOffset in EchoesModel.h)
    // rather than a local re-implementation, and pins the exact contract values.
    // Spread scaling happens at the call site (PluginProcessor / SpatialHeadView),
    // so raw offsets are asserted unscaled, then once spread-scaled.
    auto offsetFor = [](int tapIndex, int mode, float spreadPercent) -> float
    {
        const MotionTapParams params{spreadPercent, mode};
        return calculateTapPhaseOffset(tapIndex, params);
    };

    auto near = [](float a, float b) { return std::abs(a - b) < 1.0e-4f; };

    // Unison: zero offset for every tap.
    const bool unisonOk =
        near(offsetFor(0, 0, 50.0f), 0.0f) && near(offsetFor(1, 0, 50.0f), 0.0f) && near(offsetFor(2, 0, 50.0f), 0.0f);

    // Constellation: 0 / 120 / 240 degrees as phase cycles.
    const bool constellationOk = near(offsetFor(0, 1, 50.0f), 0.0f) && near(offsetFor(1, 1, 50.0f), 120.0f / 360.0f) &&
                                 near(offsetFor(2, 1, 50.0f), 240.0f / 360.0f);

    // Cascade: delay-ratio stagger contract {0, 0.33, 0.67}.
    const bool cascadeOk = near(offsetFor(0, 2, 50.0f), 0.0f) && near(offsetFor(1, 2, 50.0f), 0.33f) &&
                           near(offsetFor(2, 2, 50.0f), 0.67f);

    // Spread 0 disables choreography entirely (function returns 0 below the
    // 0.001 spread floor).
    const bool zeroSpreadOk = near(offsetFor(1, 1, 0.0f), 0.0f) && near(offsetFor(2, 2, 0.0f), 0.0f);

    const bool passed = unisonOk && constellationOk && cascadeOk && zeroSpreadOk;
    juce::String result;
    result << "ChoreographyOffsets: unison=" << (unisonOk ? "OK" : "FAIL")
           << " constellation=" << (constellationOk ? "OK" : "FAIL") << " cascade=" << (cascadeOk ? "OK" : "FAIL")
           << " zeroSpread=" << (zeroSpreadOk ? "OK" : "FAIL") << " " << (passed ? "[PASS]" : "[FAIL]");
    return result;
}

}  // namespace SpatialEchoTests::System

#endif
