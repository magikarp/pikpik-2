#pragma once
#include "p2_config.h"
#include "p2_texture_refs.h"
#include "p2_random.h"
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

// Original MSL constants, without putting MSL's libc headers on the host path.
#define LONG_TAU 6.2831854820251465
#define LONG_PI 3.1415926535897932
#define TAU 6.2831855f
#define PI 3.1415927f
#define HALF_PI 1.5707964f
#define HALF_PI_F64 1.5707963267948966
#define THIRD_PI 1.0471976f
#define QUARTER_PI 0.7853982f
#define SIN_2_5 0.43633234f
#define SQUARE(v) ((v) * (v))
#define IS_WITHIN_CIRCLE(x, z, radius) ((SQUARE(x) + SQUARE(z)) < SQUARE(radius))
#define FABS(v) fabsf(v)
#define M_SQRT3 1.73205f
#define DEG2RAD (1.0f / 180.0f)
#define RAD2DEG (180.0f / PI)
#define RAD2DEG_F64 57.29577951308232
#define TORADIANS(degrees) (PI * (DEG2RAD * (degrees)))
#define TODEGREES(radians) (RAD2DEG * (radians))

// Host approximation, not a bit-exact implementation of the PPC estimate.
static inline double __frsqrte(double x) { return 1.0 / sqrt(x); }
static inline float __fres(float x) { return 1.0f / x; }
static inline float __fabsf(float x) { return fabsf(x); }
static inline float dolsinf(float x) { return (float)sin((double)x); }
static inline float dolcosf(float x) { return (float)cos((double)x); }
static inline float doltanf(float x) { return (float)tan((double)x); }
static inline float dolatan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
static inline float sinf_kludge(float x) { return dolsinf(x); }
static inline float cosf_kludge(float x) { return dolcosf(x); }
static inline float tanf_kludge(float x) { return doltanf(x); }
static inline float scaleValue(float scale, float value) { return scale * value; }
static inline float dolsqrtfull(float value) { return sqrtf(value); }
static inline unsigned __cntlzw(unsigned value) { return value ? __builtin_clz(value) : 32; }

// Body for decomp functions the original binary does not contain (left empty
// as "UNUSED FUNCTION"): reaching one names it instead of returning garbage.
extern "C" __attribute__((noreturn)) void p2_unused_function(const char* file, int line, const char* name);
#define P2_UNUSED_FUNCTION() p2_unused_function(__FILE__, __LINE__, __func__)
