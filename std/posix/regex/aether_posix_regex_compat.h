/*
 * std.posix.regex — the environment the vendored FreeBSD sources compile in.
 *
 * Included first by each aether_posix_reg*.c translation unit, before the
 * FreeBSD file it wraps. It pulls in every system header those files name
 * (so their own #includes become no-ops), then replaces the locale-dependent
 * C library with fixed UTF-8 behaviour:
 *
 *   - wchar_t / wint_t become a signed 32-bit code point on every target
 *     (Windows' wchar_t is 16 bits and unsigned, which loses code points
 *     above U+FFFF and the engine's negative sentinels);
 *   - mbrtowc / wcrtomb are a strict UTF-8 decoder and encoder, whatever
 *     the process locale is (MB_CUR_MAX is 4);
 *   - towlower / towupper / isw* / wctype / iswctype come from utf8proc,
 *     which std.unicode already builds into libaether.a.
 *
 * So a pattern behaves the same on macOS, Linux, the BSDs and Windows, and
 * does not change if the host program calls setlocale(). Collation is gone:
 * ranges compare code points, [= =] and [. .] take one character or a
 * cname.h name.
 */
#ifndef AETHER_POSIX_REGEX_COMPAT_H
#define AETHER_POSIX_REGEX_COMPAT_H

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>

/* FreeBSD's lib/libc/regex/Makefile.inc builds with this: an unmatched ")"
 * in an ERE is an ordinary character, as POSIX.2 accidentally allowed. */
#define POSIX_MISTAKE

#define AETHER_POSIX_REGEX_INTERNAL
#include "aether_posix_regex.h"

/* BSD <sys/cdefs.h> spellings the sources use. */
#ifndef __unused
#  if defined(__GNUC__) || defined(__clang__)
#    define __unused __attribute__((__unused__))
#  else
#    define __unused
#  endif
#endif
#ifndef __restrict
#  define __restrict restrict
#endif
#ifndef __inline
#  define __inline inline
#endif

/* POSIX's minimum RE_DUP_MAX; FreeBSD's value. Windows has no <regex.h>. */
#undef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX 255

/* One code point, signed, 32 bits, on every target. */
typedef int32_t aether_posix_wint_t;
typedef struct { int aether_posix_unused; } aether_posix_mbstate_t;
typedef int aether_posix_wctype_t;

#undef wint_t
#undef wchar_t
#undef mbstate_t
#undef wctype_t
#define wint_t    aether_posix_wint_t
#define wchar_t   aether_posix_wint_t
#define mbstate_t aether_posix_mbstate_t
#define wctype_t  aether_posix_wctype_t

/* UTF-8 is the only encoding. */
#undef MB_CUR_MAX
#define MB_CUR_MAX 4
#ifndef MB_LEN_MAX
#  define MB_LEN_MAX 4
#endif

size_t aether_posix_mbrtowc(aether_posix_wint_t* pwc, const char* s, size_t n,
                            aether_posix_mbstate_t* ps);
size_t aether_posix_wcrtomb(char* s, aether_posix_wint_t wc, aether_posix_mbstate_t* ps);
aether_posix_wint_t aether_posix_towlower(aether_posix_wint_t c);
aether_posix_wint_t aether_posix_towupper(aether_posix_wint_t c);
aether_posix_wctype_t aether_posix_wctype(const char* name);
int aether_posix_iswctype(aether_posix_wint_t c, aether_posix_wctype_t t);
void* aether_posix_reallocarray(void* p, size_t n, size_t size);

#undef mbrtowc
#undef wcrtomb
#undef towlower
#undef towupper
#undef wctype
#undef iswctype
#undef iswalpha
#undef iswalnum
#undef iswupper
#undef iswlower
#undef reallocarray
#define mbrtowc      aether_posix_mbrtowc
#define wcrtomb      aether_posix_wcrtomb
#define towlower     aether_posix_towlower
#define towupper     aether_posix_towupper
#define wctype       aether_posix_wctype
#define iswctype     aether_posix_iswctype
#define reallocarray aether_posix_reallocarray

/* Class ids for aether_posix_wctype; 0 is "no such class". */
#define AETHER_POSIX_CT_ALNUM  1
#define AETHER_POSIX_CT_ALPHA  2
#define AETHER_POSIX_CT_BLANK  3
#define AETHER_POSIX_CT_CNTRL  4
#define AETHER_POSIX_CT_DIGIT  5
#define AETHER_POSIX_CT_GRAPH  6
#define AETHER_POSIX_CT_LOWER  7
#define AETHER_POSIX_CT_PRINT  8
#define AETHER_POSIX_CT_PUNCT  9
#define AETHER_POSIX_CT_SPACE  10
#define AETHER_POSIX_CT_UPPER  11
#define AETHER_POSIX_CT_XDIGIT 12

#define iswalpha(c) aether_posix_iswctype((c), AETHER_POSIX_CT_ALPHA)
#define iswalnum(c) aether_posix_iswctype((c), AETHER_POSIX_CT_ALNUM)
#define iswupper(c) aether_posix_iswctype((c), AETHER_POSIX_CT_UPPER)
#define iswlower(c) aether_posix_iswctype((c), AETHER_POSIX_CT_LOWER)

#endif /* AETHER_POSIX_REGEX_COMPAT_H */
