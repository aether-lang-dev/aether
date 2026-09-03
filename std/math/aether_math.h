#ifndef AETHER_MATH_H
#define AETHER_MATH_H

#include <stdint.h>   /* int64_t, for math_lrint's return type */

// Basic math operations
int math_abs_int(int x);
double math_abs_float(double x);
int math_min_int(int a, int b);
int math_max_int(int a, int b);
double math_min_float(double a, double b);
double math_max_float(double a, double b);
int math_clamp_int(int x, int min, int max);
double math_clamp_float(double x, double min, double max);

// Advanced math
double math_sqrt(double x);
double math_pow(double base, double exp);
double math_sin(double x);
double math_cos(double x);
double math_tan(double x);
double math_asin(double x);
double math_acos(double x);
double math_atan(double x);
double math_atan2(double y, double x);
double math_floor(double x);
double math_ceil(double x);
double math_round(double x);
/* Round half-to-even and return an integer (see the note in aether_math.c). */
int64_t math_lrint(double x);
double math_log(double x);
double math_log10(double x);
double math_exp(double x);

// Random numbers
void math_random_seed(unsigned int seed);
int math_random_int(int min, int max);
double math_random_float(void);

// Function-constants. Aether has no const-expression evaluator at the
// call site, so trigonometric / radian-conversion constants are
// exposed as zero-arg functions that return the value. The C lowering
// is a trivial return; any reasonable C compiler inlines it. Keeps
// the existing `math_*` convention so `math.pi()` reads alongside
// `math.sqrt(x)` etc.
double math_pi(void);
double math_tau(void);
double math_e(void);
double math_deg_to_rad(void);   // PI/180
double math_rad_to_deg(void);   // 180/PI

// Constants
#define PI 3.14159265358979323846
#define E 2.71828182845904523536

#endif // AETHER_MATH_H
