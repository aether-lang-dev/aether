#include "aether_math.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

// Basic math operations
// |x|. Aether's int wraps, and INT_MIN has no positive counterpart in an
// int, so abs_int(INT_MIN) is INT_MIN (as in Java and Go). Negated in
// unsigned arithmetic, where that is defined: `-x` overflowed, which C
// leaves undefined.
int math_abs_int(int x) {
    return x < 0 ? (int)(0u - (unsigned)x) : x;
}

double math_abs_float(double x) {
    return fabs(x);
}

int math_min_int(int a, int b) {
    return a < b ? a : b;
}

int math_max_int(int a, int b) {
    return a > b ? a : b;
}

// fmin/fmax: when one side is NaN the other is returned, whichever side it
// is on. A `<` pick returned b for min(NaN, 1) but NaN for min(1, NaN).
double math_min_float(double a, double b) {
    return fmin(a, b);
}

double math_max_float(double a, double b) {
    return fmax(a, b);
}

int math_clamp_int(int x, int min, int max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

double math_clamp_float(double x, double min, double max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

// Advanced math
double math_sqrt(double x) {
    return sqrt(x);
}

// Single precision (#2151). fminf/fmaxf rather than a `<` pick: they
// return the non-NaN operand when one side is NaN, as the double
// versions below do through fmin/fmax.
float math_sqrt_f32(float x) { return sqrtf(x); }
float math_abs_f32(float x) { return fabsf(x); }
float math_min_f32(float a, float b) { return fminf(a, b); }
float math_max_f32(float a, float b) { return fmaxf(a, b); }
float math_clamp_f32(float x, float min, float max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}
float math_sin_f32(float x) { return sinf(x); }
float math_cos_f32(float x) { return cosf(x); }
float math_tan_f32(float x) { return tanf(x); }
float math_atan2_f32(float y, float x) { return atan2f(y, x); }
float math_floor_f32(float x) { return floorf(x); }
float math_ceil_f32(float x) { return ceilf(x); }
float math_pow_f32(float base, float exp) { return powf(base, exp); }
float math_exp_f32(float x) { return expf(x); }

double math_pow(double base, double exp) {
    return pow(base, exp);
}

double math_sin(double x) {
    return sin(x);
}

double math_cos(double x) {
    return cos(x);
}

double math_tan(double x) {
    return tan(x);
}

double math_asin(double x) {
    return asin(x);
}

double math_acos(double x) {
    return acos(x);
}

double math_atan(double x) {
    return atan(x);
}

double math_atan2(double y, double x) {
    return atan2(y, x);
}

double math_floor(double x) {
    return floor(x);
}

double math_ceil(double x) {
    return ceil(x);
}

double math_round(double x) {
    return round(x);
}

/* Round to nearest and return an INTEGER type, unlike math_round which rounds
 * correctly but hands back a double that the caller must then cast.
 *
 * This exists so callers do not declare `extern lrint` themselves. An Aether
 * extern cannot spell libm's prototype: `-> long` emits int64_t and `-> int`
 * emits int, and C `long` is neither, so every such declaration collides with
 * <math.h> and clang warns (-Wincompatible-library-redeclaration) in every
 * generated program that uses it. Declaring it once here, in C, against the
 * real header is the only place the prototype can be correct.
 *
 * int64_t, not long, is the return type: it is what an Aether `-> long` binds
 * to, and it is the same width on every LP64 target while being well-defined
 * on LLP64 (Windows) where long is 32 bits and would silently narrow.
 *
 * NB this rounds half-to-EVEN (lrint honours the current rounding mode, which
 * defaults to FE_TONEAREST), whereas math_round rounds half-away-from-zero:
 * math_lrint(0.5) == 0 but math_round(0.5) == 1.0. That is the documented
 * difference between the two, not an accident — callers converting a
 * hand-rolled `extern lrint` keep their existing behaviour by using this. */
int64_t math_lrint(double x) {
    return (int64_t)llrint(x);
}

double math_log(double x) {
    return log(x);
}

double math_log10(double x) {
    return log10(x);
}

double math_exp(double x) {
    return exp(x);
}

// Random numbers

/* splitmix64 (Steele, Lea, Flood 2014): a 64-bit state, one add and three
 * mixing steps per draw, full period, and well distributed in every bit. It
 * replaces rand(), whose RAND_MAX is 32767 on Windows: random_int(0, 1000000)
 * never went above 32767 there, `rand() % range` was biased, and
 * `max - min + 1` over the whole int range overflowed to 0 and divided by
 * zero. Same seed, same sequence, on every platform.
 *
 * The state is one process-wide word that actors draw from on their worker
 * threads (rand() took a lock), so the step is an atomic add: two threads
 * drawing at once get two distinct steps of the sequence instead of one
 * lost update and the same number twice. For one thread the sequence after
 * random_seed is the same as before. The first draw seeds from the clock
 * once, whichever thread gets there first. */
static _Atomic uint64_t random_state = 0;
static atomic_int random_initialized = 0;

#define RANDOM_GOLDEN 0x9E3779B97F4A7C15ULL

static uint64_t random_next(void) {
    uint64_t z = atomic_fetch_add_explicit(&random_state, RANDOM_GOLDEN,
                                           memory_order_relaxed) + RANDOM_GOLDEN;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void math_random_seed(unsigned int seed) {
    atomic_store_explicit(&random_state, (uint64_t)seed, memory_order_relaxed);
    atomic_store_explicit(&random_initialized, 1, memory_order_release);
}

static void random_seed_once(void) {
    int expected = 0;
    if (atomic_load_explicit(&random_initialized, memory_order_acquire)) return;
    /* The first caller seeds; a second one arriving meanwhile draws from
     * the seeded state rather than resetting it. */
    if (atomic_compare_exchange_strong(&random_initialized, &expected, 2)) {
        atomic_store_explicit(&random_state, (uint64_t)time(NULL), memory_order_relaxed);
        atomic_store_explicit(&random_initialized, 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&random_initialized, memory_order_acquire) != 1) { }
    }
}

/* A uniform int in [min, max], both ends included. The span is computed in
 * 64 bits (at most 2^32, so it cannot overflow), and draws below
 * 2^64 mod span are rejected so every value is equally likely. */
int math_random_int(int min, int max) {
    random_seed_once();
    if (min >= max) return min;
    uint64_t span = (uint64_t)((int64_t)max - (int64_t)min) + 1u;
    uint64_t floor_ = (0u - span) % span;
    uint64_t r;
    do { r = random_next(); } while (r < floor_);
    return (int)((int64_t)min + (int64_t)(r % span));
}

/* A uniform float in [0, 1): the top 53 bits of a draw, scaled by 2^-53,
 * so every representable step is equally likely and 1.0 is never returned
 * (`floor(random_float() * n)` stays below n). */
double math_random_float(void) {
    random_seed_once();
    return (double)(random_next() >> 11) * (1.0 / 9007199254740992.0);
}

// Function-constants — see header comment.
double math_pi(void)         { return 3.14159265358979323846; }
double math_tau(void)        { return 6.28318530717958647692; }
double math_e(void)          { return 2.71828182845904523536; }
double math_deg_to_rad(void) { return 0.017453292519943295; }  /* PI/180 */
double math_rad_to_deg(void) { return 57.29577951308232; }     /* 180/PI */
