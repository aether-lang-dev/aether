#include "aether_math.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>

// Basic math operations
int math_abs_int(int x) {
    return x < 0 ? -x : x;
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

double math_min_float(double a, double b) {
    return a < b ? a : b;
}

double math_max_float(double a, double b) {
    return a > b ? a : b;
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
static int random_initialized = 0;

void math_random_seed(unsigned int seed) {
    srand(seed);
    random_initialized = 1;
}

int math_random_int(int min, int max) {
    if (!random_initialized) {
        math_random_seed((unsigned int)time(NULL));
    }
    if (min >= max) return min;
    return min + (rand() % (max - min + 1));
}

double math_random_float(void) {
    if (!random_initialized) {
        math_random_seed((unsigned int)time(NULL));
    }
    return (double)rand() / (double)RAND_MAX;
}

// Function-constants — see header comment.
double math_pi(void)         { return 3.14159265358979323846; }
double math_tau(void)        { return 6.28318530717958647692; }
double math_e(void)          { return 2.71828182845904523536; }
double math_deg_to_rad(void) { return 0.017453292519943295; }  /* PI/180 */
double math_rad_to_deg(void) { return 57.29577951308232; }     /* 180/PI */
