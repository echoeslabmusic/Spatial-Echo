// DelayHeadActivation: Map the DelaySetting choice to audible echo heads.
// Realtime-safety: constexpr, allocation-free helpers only.

#pragma once

#include <array>

namespace DelayHeadActivation
{
using Mask = std::array<bool, 3>;

constexpr Mask activeHeadsForDelayMode(int delayMode) noexcept
{
    switch (delayMode)
    {
        case 0:
            return {true, false, false};  // H1 Echo
        case 1:
            return {false, true, false};  // H2 Echo
        case 2:
            return {false, false, true};  // H3 Echo
        case 3:
            return {false, true, true};  // H2+H3 Echo
        case 4:
            return {true, false, false};  // H1 Echo+Verb
        case 5:
            return {false, true, false};  // H2 Echo+Verb
        case 6:
            return {false, false, true};  // H3 Echo+Verb
        case 7:
            return {true, true, false};  // H1+H2 Echo+Verb
        case 8:
            return {false, true, true};  // H2+H3 Echo+Verb
        case 9:
            return {true, false, true};  // H1+H3 Echo+Verb
        case 10:
            return {true, true, true};  // All Heads Echo+Verb
        case 11:
            return {false, false, false};  // Reverb Only
        default:
            return {true, true, true};  // Fail open for unknown state
    }
}

constexpr bool anyActive(const Mask& activeHeads) noexcept
{
    return activeHeads[0] || activeHeads[1] || activeHeads[2];
}

constexpr int firstActiveIndex(const Mask& activeHeads) noexcept
{
    for (int index = 0; index < static_cast<int>(activeHeads.size()); ++index)
        if (activeHeads[static_cast<std::size_t>(index)])
            return index;
    return -1;
}
}  // namespace DelayHeadActivation
