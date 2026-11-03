// SpatialMotionTrajectory.h: Pure function trajectory evaluator for spatial motion patterns.
// Realtime-safety: yes (pure function, no state or allocations) API: spatialmotion::evaluateTrajectory(pattern,
//   phaseCycles, depth01)

#pragma once

#include "../SpatialParamIDs.h"
#include "SpatialMotionLfo.h"
#include <cmath>
#include <cstdint>

namespace spatialmotion
{

// Output structure for trajectory evaluation
struct TrajectoryOffsets
{
    float azimuthDeg = 0.0f;      // Azimuth offset in degrees (-180..+180 range)
    float elevationDeg = 0.0f;    // Elevation offset in degrees (-90..+90 range)
    float distanceMeters = 0.0f;  // Distance offset in meters
};

namespace detail
{

// Catmull-Rom spline interpolation (Wander pattern uses this)
inline float catmullRomSpline(float t, float p0, float p1, float p2, float p3)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// Linear interpolation helper
inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Clamp helper
inline float clamp(float value, float minVal, float maxVal)
{
    return (value < minVal) ? minVal : (value > maxVal) ? maxVal : value;
}

}  // namespace detail

// ============================================================================
// Pattern Evaluators
// ============================================================================

// Still pattern: returns exactly zero offsets for any phase and depth
inline TrajectoryOffsets evaluateStill(double /*phaseCycles*/, float /*depth01*/)
{
    return TrajectoryOffsets{0.0f, 0.0f, 0.0f};
}

// Orbit: TRUE continuous rotation in azimuth, with depth scaling elevation tilt
// and radial excursion. Full 360° per cycle, seamless wrap across +/-180°.
inline TrajectoryOffsets evaluateOrbit(double phaseCycles, float depth01)
{
    depth01 = detail::clamp(depth01, 0.0f, 1.0f);

    // Azimuth: continuous rotation 360° per cycle
    float rawPhase = static_cast<float>(phaseCycles - std::floor(phaseCycles));
    float azimuthDeg = rawPhase * 360.0f - 180.0f;  // Map [0,1) to [-180, 180)

    // Elevation: +/- 45° tilt at full depth (depth01=1.0), one sine cycle per orbit
    float elevationAmount = 45.0f * depth01;
    float elevationDeg = elevationAmount * std::sin(2.0f * 3.14159265359f * rawPhase);

    // Distance: +/- 0.5m radial excursion at full depth
    float distanceAmount = 0.5f * depth01;
    float distanceMeters = distanceAmount * std::cos(2.0f * 3.14159265359f * rawPhase);

    return TrajectoryOffsets{azimuthDeg, elevationDeg, distanceMeters};
}

// Pendulum: eased triangle wave azimuth swing, +/-60° at full depth
inline TrajectoryOffsets evaluatePendulum(double phaseCycles, float depth01)
{
    depth01 = detail::clamp(depth01, 0.0f, 1.0f);

    float rawPhase = static_cast<float>(phaseCycles - std::floor(phaseCycles));

    // Triangle wave: 0 -> 1 -> 0 over the cycle
    float trianglePhase = (rawPhase < 0.5f) ? (2.0f * rawPhase) : (2.0f * (1.0f - rawPhase));

    // Ease with cubic smoothstep: smoother acceleration/deceleration
    float eased = trianglePhase * trianglePhase * (3.0f - 2.0f * trianglePhase);

    // Swing +/- 60° at full depth, mapped from -1..+1 eased range to -60..+60
    float swingAmount = 60.0f * depth01;
    float azimuthDeg = swingAmount * (2.0f * eased - 1.0f);

    return TrajectoryOffsets{azimuthDeg, 0.0f, 0.0f};
}

// Figure8: lissajous with elevation at 2x azimuth phase (reads as an 8)
inline TrajectoryOffsets evaluateFigure8(double phaseCycles, float depth01)
{
    depth01 = detail::clamp(depth01, 0.0f, 1.0f);

    float rawPhase = static_cast<float>(phaseCycles - std::floor(phaseCycles));

    // Azimuth: sine wave, full swing +/-60° at depth
    float azimuthDeg = 60.0f * depth01 * std::sin(2.0f * 3.14159265359f * rawPhase);

    // Elevation: sine at 2x frequency (creates the "8" shape)
    // Range +/-60° at full depth
    float elevationDeg = 60.0f * depth01 * std::sin(4.0f * 3.14159265359f * rawPhase);

    return TrajectoryOffsets{azimuthDeg, elevationDeg, 0.0f};
}

// Breathe: distance-only sine modulation, log-domain safe, +/-1.5m at full depth
inline TrajectoryOffsets evaluateBreathe(double phaseCycles, float depth01)
{
    depth01 = detail::clamp(depth01, 0.0f, 1.0f);

    float rawPhase = static_cast<float>(phaseCycles - std::floor(phaseCycles));

    // Pure sine in distance domain
    // Center at 0m, amplitude +/- 1.5m at full depth
    float distanceMeters = 1.5f * depth01 * std::sin(2.0f * 3.14159265359f * rawPhase);

    return TrajectoryOffsets{0.0f, 0.0f, distanceMeters};
}

// Wander: smoothed random walk (value-noise via Catmull-Rom), deterministic
// (fixed seed keyed only on phase). Slew is baked into the spline
// interpolation on every axis.
inline TrajectoryOffsets evaluateWander(double phaseCycles, float depth01)
{
    depth01 = detail::clamp(depth01, 0.0f, 1.0f);

    // Fixed seed, so the same phase always yields the same offsets.
    constexpr float kSeed = 0.31f;

    const int step = static_cast<int>(std::floor(phaseCycles));
    const float rawPhase = static_cast<float>(phaseCycles - std::floor(phaseCycles));

    // Each axis is Catmull-Rom value noise over its own decorrelated step
    // series - continuous across every integer phase boundary.
    auto noiseAt = [&](int channelOffset)
    {
        const int s = step + channelOffset;
        return detail::catmullRomSpline(
            rawPhase, SpatialMotionLfo::hashSigned(s - 1, kSeed), SpatialMotionLfo::hashSigned(s, kSeed),
            SpatialMotionLfo::hashSigned(s + 1, kSeed), SpatialMotionLfo::hashSigned(s + 2, kSeed));
    };

    return TrajectoryOffsets{60.0f * depth01 * noiseAt(0), 45.0f * depth01 * noiseAt(100),
                             1.5f * depth01 * noiseAt(200)};
}

// ============================================================================
// Main Evaluator
// ============================================================================

/**
 * Pure function mapping (pattern, phaseCycles, depth) -> spatial offsets.
 *
 * @param pattern        Selected motion pattern (Still, Orbit, Pendulum, Figure8, Breathe, Wander)
 * @param phaseCycles    Accumulating phase in cycles (wraps at integer boundaries)
 * @param depth01        Depth scalar [0, 1]: 0 = no motion, 1 = full amplitude
 * @return               Spatial offsets: azimuth, elevation, distance
 *
 * Properties:
 *   - depth01=0 always returns exactly {0, 0, 0} for every pattern
 *   - Pure function: identical calls yield identical results (Wander is deterministic)
 *   - No hidden state; no allocation; real-time safe
 *   - Caller applies per-tap choreography phase offsets before invoking
 */
inline TrajectoryOffsets evaluateTrajectory(SpatialIDs::motion::Pattern pattern, double phaseCycles, float depth01)
{
    // Fast-path: depth=0 always returns silence
    if (depth01 <= 0.0f)
        return TrajectoryOffsets{0.0f, 0.0f, 0.0f};

    switch (pattern)
    {
        case SpatialIDs::motion::Pattern::Still:
            return evaluateStill(phaseCycles, depth01);
        case SpatialIDs::motion::Pattern::Orbit:
            return evaluateOrbit(phaseCycles, depth01);
        case SpatialIDs::motion::Pattern::Pendulum:
            return evaluatePendulum(phaseCycles, depth01);
        case SpatialIDs::motion::Pattern::Figure8:
            return evaluateFigure8(phaseCycles, depth01);
        case SpatialIDs::motion::Pattern::Breathe:
            return evaluateBreathe(phaseCycles, depth01);
        case SpatialIDs::motion::Pattern::Wander:
            return evaluateWander(phaseCycles, depth01);
        default:
            return TrajectoryOffsets{0.0f, 0.0f, 0.0f};
    }
}

}  // namespace spatialmotion
