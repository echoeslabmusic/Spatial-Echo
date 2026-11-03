#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifndef SAF_UNUSED
#define SAF_UNUSED(x) (void)(x)
#endif

#ifndef SAF_MIN
#define SAF_MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef SAF_MAX
#define SAF_MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef SAF_CLAMP
#define SAF_CLAMP(a,min,max) (SAF_MAX((min), SAF_MIN((max), (a))))
#endif

#ifndef SAF_PI
#define SAF_PI (3.14159265358979323846264338327950288f)
#endif
