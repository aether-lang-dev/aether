// Aether — locale-independent float text conversion
//
// MACHINE TEXT ONLY. Every conversion here behaves as if the process were in
// the "C" locale: the decimal separator is always '.', and digits are never
// grouped. Use these for wire formats, JSON, config files, string
// interpolation — anything that must be byte-identical on every machine
// regardless of the ambient locale.
//
// HUMAN-FACING formatting is the opposite requirement and belongs in
// std.number (issue #863 Phase 4), which takes an explicit locale argument.
// Do NOT route std.number through this header.
//
// Why this file exists: libc's snprintf/strtod/strtof obey LC_NUMERIC. A C
// program starts in the "C" locale, so a standalone Aether binary is safe by
// default — but an Aether library embedded in a host that has called
// setlocale(LC_ALL, "") (GTK, PHP, countless C/C++ apps) inherits that host's
// locale, and then string.to_double("3.14") FAILS to parse under any
// comma-decimal locale. Fixing that by calling setlocale ourselves is not an
// option: it is process-global, it is not ours to change as a library, and
// Aether runs its own threads (actors, scheduler, worker pool, HTTP accept
// threads) so mutating it would be a data race in our own runtime.
//
// Instead we pin the locale per call, by the cheapest mechanism the platform
// offers — see AETHER_HAS_LOCALE_CONV in the .c file.

#ifndef AETHER_LOCALE_NUM_H
#define AETHER_LOCALE_NUM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Format `value` into `buf` using `fmt`, always with '.' as the decimal
// separator. `fmt` must be a single float conversion — "%g", "%.17g", "%f"
// and friends; this is NOT a general printf passthrough (no extra arguments
// are forwarded).
//
// Returns C99 snprintf semantics on EVERY platform: the number of characters
// that would have been written excluding the NUL, or negative on encoding
// error. `buf` is always NUL-terminated when `n > 0` — including on
// truncation, where Windows' _snprintf_l would otherwise leave it unterminated.
int aether_c_snprintf_double(char* buf, size_t n, const char* fmt, double value);

// strtod/strtof with '.' as the decimal separator regardless of locale.
// `errno` and `endptr` follow the standard functions exactly, so existing
// ERANGE and trailing-garbage checks keep working unchanged.
double aether_c_strtod(const char* s, char** endptr);
float  aether_c_strtof(const char* s, char** endptr);

// TEST-ONLY. Sets the PROCESS-GLOBAL locale via setlocale(LC_ALL, name) —
// precisely the thing the rest of this header exists to avoid doing. It is
// here so locale regression tests can reproduce the embedded-host scenario
// in-process; call it only from a single-threaded test main, before any
// actor, scheduler or worker thread exists.
//
// Returns 1 if the locale was applied, 0 if it is not installed on this
// machine (callers should print a visible SKIP and pass).
int aether_test_setlocale(const char* name);

#ifdef __cplusplus
}
#endif

#endif // AETHER_LOCALE_NUM_H
