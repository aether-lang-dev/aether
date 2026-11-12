# FreeBSD libc regex, vendored for std.posix.regex

Henry Spencer's POSIX regular-expression engine as FreeBSD's libc carries it,
from `lib/libc/regex/` of <https://github.com/freebsd/freebsd-src>.

- Commit: `9d2cdb3fd5741685622b0b9c0e17e22583c9ceff` (main, 2026-11-12)
- Files: `regcomp.c`, `regexec.c`, `engine.c`, `regerror.c`, `regfree.c`,
  `regex2.h`, `utils.h`, `cname.h`, and `COPYRIGHT` unchanged.
- Licence: BSD-3-Clause (each file's SPDX header), and Henry Spencer's
  notice in `COPYRIGHT`. Both are reproduced in `THIRD_PARTY_LICENSES.md`.
  Spencer's notice asks that altered versions be plainly marked: every change
  below carries an `AETHER:` comment at the place it was made.
- Built as FreeBSD builds it (`lib/libc/regex/Makefile.inc`: `-DPOSIX_MISTAKE`),
  without `LIBREGEX` (FreeBSD's separate GNU-extension library) and without
  `REDEBUG`.

## Changes

The engine is trimmed to UTF-8 only and renamed, nothing else.

1. `#include <regex.h>` is `#include "../aether_posix_regex.h"` in
   `regcomp.c`, `regexec.c`, `regerror.c` and `regfree.c`. That header
   declares the prefixed names (`aether_posix_regcomp`, `aether_posix_regex_t`,
   `AETHER_POSIX_REG_EXTENDED`, ...) and, for these sources only, maps the
   classic names onto them, so libaether.a exports no `regcomp` to clash with
   a host libc's.
2. `regcomp.c`: the `collate.h` include, `p_range_cmp()` (collation-order
   comparison) and the single-byte-locale branch of `p_b_term()` that used it
   are removed; a range is always the code-point range FreeBSD uses when
   `MB_CUR_MAX > 1`. The `_CurrentRuneLocale` "is it UTF-8?" check in
   `findmust()` is removed (it always is). `freebsd12_regcomp` and its
   `__sym_compat` (a symbol-versioned compatibility entry) are removed.
3. `regexec.c`: only the multibyte instantiation of `engine.c` (`mmatcher`)
   is built, and `regexec()` always calls it, as FreeBSD's does whenever
   `MB_CUR_MAX > 1`. The single-byte `smatcher` and `lmatcher` are not built.
4. `utils.h`: `NC` is 128, its UTF-8 value.

Nothing in `engine.c`, `regerror.c`, `regfree.c`, `regex2.h` or `cname.h`
changes beyond item 1.

The locale-dependent C library the sources call is replaced, not edited out:
`../aether_posix_regex_compat.h` (included before each source by
`../aether_posix_reg*.c`) maps `mbrtowc`, `wcrtomb`, `towlower`, `towupper`,
`iswctype`, `wctype`, `isw*`, `wint_t`, `wchar_t` and `mbstate_t` onto a fixed
UTF-8 implementation in `../aether_posix_regex.c` (character data from
utf8proc, which std.unicode already ships), and supplies `reallocarray`,
`__unused`, `__restrict` and `_POSIX2_RE_DUP_MAX` where a target lacks them.

## Re-vendoring

Copy the files above from the new commit, re-apply the four changes (each is
marked `AETHER:`), update the commit here, and run
`std/posix/regex/test_posix_regex_freebsd.ae`, `test_posix_regex_att.ae`,
`test_posix_regex.ae` and `tests/integration/posix_regex_system/`.
