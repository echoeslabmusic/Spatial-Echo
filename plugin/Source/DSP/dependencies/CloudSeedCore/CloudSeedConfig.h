// CloudSeedCore configuration for Spatial Echo integration.
// This header must be included (or the macros defined via compiler flags)
// before any CloudSeedCore DSP headers that use BUFFER_SIZE.

#pragma once

// Ensure M_PI is available even if <cmath> was already included
// (MSVC requires _USE_MATH_DEFINES before <cmath>, but JUCE includes it first)
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef BUFFER_SIZE
#define BUFFER_SIZE 1024
#endif

#ifndef MAX_STR_SIZE
#define MAX_STR_SIZE 32
#endif
