/* aether_math_inline.h: std.math's scalar functions as `static inline`
 * (#3852).
 *
 * std.math asks for this header with `@c_include` (the #1986 mechanism, as
 * std.string does since #3836), so in a program that imports it
 * `math.sqrt(x)`, `math.min_float(a, b)` and the rest below are the
 * operations they name rather than calls into libaether.a the C compiler
 * cannot see through: `math.sqrt` was a call to a function that called
 * libm's sqrt, where the compiler emits one instruction (a sqrt and a
 * check of its operand, keeping errno as libm sets it).
 *
 * ONE BODY. aether_math.c includes this header, and the exported math_*
 * functions there are these, so C that calls the exported names and an
 * Aether program that calls the inline ones compute the same values: the
 * same libm semantics (fmin/fmax return the non-NaN operand, abs_int of
 * INT_MIN is INT_MIN), the same rounding.
 *
 * Injected into the translation unit of every program that imports
 * std.math, so it declares nothing that is not prefixed (the lesson of
 * #2162) and does not include <math.h> on GCC and clang: generated C does
 * not include it there, and a program may define a function of its own
 * with one of libm's names. The compiler's builtins are those functions
 * without a declaration. Other compilers (cl.exe) get <math.h>, which
 * generated C already includes for them. Transcendental functions (sin,
 * pow, exp...) stay out of line: their cost is the libm call itself. */
#ifndef AETHER_MATH_INLINE_H
#define AETHER_MATH_INLINE_H

#if defined(__GNUC__) || defined(__clang__)
#define AETHER_MATH_SQRT(x)   __builtin_sqrt(x)
#define AETHER_MATH_SQRTF(x)  __builtin_sqrtf(x)
#define AETHER_MATH_FABS(x)   __builtin_fabs(x)
#define AETHER_MATH_FABSF(x)  __builtin_fabsf(x)
#define AETHER_MATH_FMIN(a, b)  __builtin_fmin((a), (b))
#define AETHER_MATH_FMAX(a, b)  __builtin_fmax((a), (b))
#define AETHER_MATH_FMINF(a, b) __builtin_fminf((a), (b))
#define AETHER_MATH_FMAXF(a, b) __builtin_fmaxf((a), (b))
#define AETHER_MATH_FLOOR(x)  __builtin_floor(x)
#define AETHER_MATH_FLOORF(x) __builtin_floorf(x)
#define AETHER_MATH_CEIL(x)   __builtin_ceil(x)
#define AETHER_MATH_CEILF(x)  __builtin_ceilf(x)
#define AETHER_MATH_ROUND(x)  __builtin_round(x)
#else
#include <math.h>
#define AETHER_MATH_SQRT(x)   sqrt(x)
#define AETHER_MATH_SQRTF(x)  sqrtf(x)
#define AETHER_MATH_FABS(x)   fabs(x)
#define AETHER_MATH_FABSF(x)  fabsf(x)
#define AETHER_MATH_FMIN(a, b)  fmin((a), (b))
#define AETHER_MATH_FMAX(a, b)  fmax((a), (b))
#define AETHER_MATH_FMINF(a, b) fminf((a), (b))
#define AETHER_MATH_FMAXF(a, b) fmaxf((a), (b))
#define AETHER_MATH_FLOOR(x)  floor(x)
#define AETHER_MATH_FLOORF(x) floorf(x)
#define AETHER_MATH_CEIL(x)   ceil(x)
#define AETHER_MATH_CEILF(x)  ceilf(x)
#define AETHER_MATH_ROUND(x)  round(x)
#endif

/* |x|. Aether's int wraps, and INT_MIN has no positive counterpart in an
 * int, so abs_int(INT_MIN) is INT_MIN (as in Java and Go). Negated in
 * unsigned arithmetic, where that is defined: `-x` overflowed, which C
 * leaves undefined. */
static inline int aether_math_abs_int_inline(int x) {
    return x < 0 ? (int)(0u - (unsigned)x) : x;
}
static inline double aether_math_abs_float_inline(double x) { return AETHER_MATH_FABS(x); }
static inline int aether_math_min_int_inline(int a, int b) { return a < b ? a : b; }
static inline int aether_math_max_int_inline(int a, int b) { return a > b ? a : b; }

/* fmin/fmax: when one side is NaN the other is returned, whichever side it
 * is on. A `<` pick returned b for min(NaN, 1) but NaN for min(1, NaN). */
static inline double aether_math_min_float_inline(double a, double b) { return AETHER_MATH_FMIN(a, b); }
static inline double aether_math_max_float_inline(double a, double b) { return AETHER_MATH_FMAX(a, b); }

static inline int aether_math_clamp_int_inline(int x, int min, int max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}
static inline double aether_math_clamp_float_inline(double x, double min, double max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

static inline double aether_math_sqrt_inline(double x) { return AETHER_MATH_SQRT(x); }
static inline double aether_math_floor_inline(double x) { return AETHER_MATH_FLOOR(x); }
static inline double aether_math_ceil_inline(double x) { return AETHER_MATH_CEIL(x); }
static inline double aether_math_round_inline(double x) { return AETHER_MATH_ROUND(x); }

/* Single precision (#2151). fminf/fmaxf rather than a `<` pick: they
 * return the non-NaN operand when one side is NaN, as the double versions
 * do through fmin/fmax. */
static inline float aether_math_sqrt_f32_inline(float x) { return AETHER_MATH_SQRTF(x); }
static inline float aether_math_abs_f32_inline(float x) { return AETHER_MATH_FABSF(x); }
static inline float aether_math_min_f32_inline(float a, float b) { return AETHER_MATH_FMINF(a, b); }
static inline float aether_math_max_f32_inline(float a, float b) { return AETHER_MATH_FMAXF(a, b); }
static inline float aether_math_clamp_f32_inline(float x, float min, float max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}
static inline float aether_math_floor_f32_inline(float x) { return AETHER_MATH_FLOORF(x); }
static inline float aether_math_ceil_f32_inline(float x) { return AETHER_MATH_CEILF(x); }

#endif /* AETHER_MATH_INLINE_H */
