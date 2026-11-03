// DeterministicTestMode: Determinism switch for reproducible test renders. Only test-capable builds (JUCE_DEBUG or
//   SPATIAL_ECHO_ENABLE_HEADLESS_TESTS) read the environment; a shipping Release plug-in always runs with live seeds.

#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>

#ifndef SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#define SPATIAL_ECHO_ENABLE_HEADLESS_TESTS 0
#endif

#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
#include <cstdlib>
#endif

namespace SpatialEchoDeterminism
{
#if JUCE_DEBUG || SPATIAL_ECHO_ENABLE_HEADLESS_TESTS
// True when SPATIAL_ECHO_DETERMINISTIC, SPATIAL_ECHO_RUN_TEST or
// SPATIAL_ECHO_HEADLESS_TEST is set to a non-empty value. Read once per process.
inline bool isDeterministicTestMode()
{
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;

    const char* forced = std::getenv("SPATIAL_ECHO_DETERMINISTIC");
    const char* runTest = std::getenv("SPATIAL_ECHO_RUN_TEST");
    const char* headless = std::getenv("SPATIAL_ECHO_HEADLESS_TEST");
    cached = ((forced && *forced) || (runTest && *runTest) || (headless && *headless)) ? 1 : 0;
    return cached != 0;
}
#else
constexpr bool isDeterministicTestMode() noexcept
{
    return false;
}
#endif

constexpr uint32_t kSeedBase = 0xE226C0DEu;
}  // namespace SpatialEchoDeterminism
