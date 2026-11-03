// SpatialMotionLfo: Deterministic hash shared by the spatial motion randomness (Random retrigger, Wander pattern).

#pragma once

#include <cmath>

namespace SpatialMotionLfo
{
// Maps an integer step and a seed to a repeatable value in [-1, 1).
inline float hashSigned(int step, float seed)
{
    const float v = std::sin((static_cast<float>(step) + 1.0f) * 12.9898f + seed * 78.233f) * 43758.5453f;
    const float frac = v - std::floor(v);
    return frac * 2.0f - 1.0f;
}
}  // namespace SpatialMotionLfo
