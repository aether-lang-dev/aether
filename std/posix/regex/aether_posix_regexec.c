/*
 * std.posix.regex — FreeBSD's regexec.c (freebsd/, see PROVENANCE.md there),
 * compiled in the fixed UTF-8 environment of aether_posix_regex_compat.h with
 * its symbols prefixed aether_posix_*. One translation unit per FreeBSD file:
 * the sources share headers with no include guards, as libc builds them.
 */
#include "aether_posix_regex_compat.h"
/* FreeBSD builds these sources with its own warning set; gcc's -Wextra adds
 * two it does not: int code points compared with the unsigned strip operand
 * (sign-compare), and a may-be-uninitialized false positive once the
 * bracket parser is inlined. Silenced here rather than edited upstream. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "freebsd/regexec.c"
