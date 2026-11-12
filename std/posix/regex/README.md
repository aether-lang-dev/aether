# std.posix.regex

POSIX regular expressions: Basic (BRE, the default) and Extended (ERE) syntax,
POSIX leftmost-longest matching, and back-references. The engine is Henry
Spencer's, as FreeBSD's libc carries it, vendored in `freebsd/` (see
`freebsd/PROVENANCE.md`), so the same engine runs on every target, Windows
included, where there is no `regcomp` of the system's to fall back on.

`std.regex` (PCRE2) is the general-purpose engine, with lookaround, lazy
quantifiers, named groups and Unicode properties. This module is for POSIX
tools (sed, grep, awk) and faithful ports of C that calls `regcomp`/`regexec`.
The two pick different matches once a pattern has alternation: POSIX takes the
longest match at the leftmost position, PCRE2 the first alternative that
matches.

```aether,run
import std.posix.regex

main() {
    // ERE: "a|ab" on "abcd" is "ab", the longest; std.regex would give "a".
    re = regex.compile("a|ab", regex.EXTENDED) or {
        println("pattern did not compile")
        return
    }
    m, _ = regex.exec(re, "abcd", 0)
    if m != null {
        println("ERE match ${regex.match_start(m, 0)}..${regex.match_end(m, 0)}")
        regex.match_free(m)
    }
    regex.free(re)

    // BRE: \( \) group, \1 refers back to the group.
    b = regex.compile("\\(ab*\\)c\\1", regex.BASIC) or { return }
    bm, _ = regex.exec(b, "xabbcabb", 0)
    if bm != null {
        println("BRE match ${regex.match_start(bm, 0)}..${regex.match_end(bm, 0)}, group 1 ${regex.match_start(bm, 1)}..${regex.match_end(bm, 1)}")
        regex.match_free(bm)
    }
    regex.free(b)

    // A malformed pattern is a value, with regerror's text.
    _bad, err = regex.compile("a\\(b", regex.BASIC)
    println("error: ${err}")
}
```
```output
ERE match 0..2
BRE match 1..8, group 1 1..4
error: parentheses not balanced
```

## Flags

`compile(pattern, flags)` takes regcomp's flags, OR-ed together: `BASIC` (0,
the default), `EXTENDED`, `ICASE`, `NOSUB` (match or not, no offsets),
`NEWLINE` (a newline separates lines: `.` and `[^...]` do not match it, `^`
and `$` match beside it), and FreeBSD's `NOSPEC` (the whole pattern is a
literal string; not with `EXTENDED`).

The exec functions take regexec's: `NOTBOL` (the start of the subject is not
the start of a line) and `NOTEOL`.

## Matching

- `exec(re, subject, eflags)` returns `(m, "")` on a match, `(null, "")` when
  there is none, and `(null, err)` if the engine fails. `m` is a new match the
  caller frees with `match_free`.
- `match_new(re)` makes a match buffer to reuse across `exec_into(re, subject,
  eflags, m)` and `exec_range(re, subject, start, end, eflags, m)` calls, which
  return `(1, "")` on a match and `(0, "")` on none. `exec_range` matches only
  the bytes `[start, end)` (`end` -1 for the end), regexec's `REG_STARTEND`,
  and reports offsets from the start of `subject`; a scan over every match in a
  string walks `start` forward this way without copying.
- `match_count(m)` is `nsub(re) + 1` after a match (0 for a `NOSUB` pattern).
  `match_start(m, i)` and `match_end(m, i)` are byte offsets of slot `i`, 0
  being the whole match; both are -1 for a group that took no part.
- `matches(re, subject, eflags)` is 1 or 0.

Offsets are bytes. Subjects and patterns are length-aware, so a NUL is an
ordinary byte in either.

## UTF-8, and no locale

Patterns and subjects are UTF-8 and the module ignores the process locale: a
character is a code point, so `.` matches `é` (two bytes), `[à-ä]` is a range
of code points, and `ICASE` folds with Unicode's simple case mappings. The
classes (`[:alpha:]`, `[:space:]` and the rest) are the C locale's for ASCII
and Unicode's general categories beyond it, with `[:digit:]` and `[:xdigit:]`
ASCII-only as POSIX asks. As in FreeBSD's own UTF-8 locales, a byte of the
subject that is not valid UTF-8 is matched by neither `.` nor a bracket
expression, and one in a pattern is an error ("illegal byte sequence"). Collating elements are single
characters or the POSIX names (`[[.hyphen.]]`); there is no collation order.

## Threads and ownership

There is no module or thread-local state. A compiled pattern and a match are
handles the caller owns, freed with `free` and `match_free`; separate handles
are independent and can be used on different threads at once, but one handle
must not be used by two threads at the same time.

## Tests

- `test_posix_regex.ae`: the API, `tests/conformance.txt` (POSIX cases with
  FreeBSD's answers), and `a|ab` against `std.regex`.
- `test_posix_regex_freebsd.ae`: FreeBSD's regex regression data
  (`tests/data/*.in`) with its driver ported.
- `test_posix_regex_att.ae`: the AT&T conformance data FreeBSD runs, with its
  known-bug list.
- `tests/integration/posix_regex_system/`: `tests/conformance.txt` against the
  host's own `regcomp` on FreeBSD and macOS.

## Credits

The engine is Henry Spencer's (Copyright 1992, 1993, 1994 Henry Spencer, and
the Regents of the University of California), with later work by the FreeBSD
project, under BSD-3-Clause; see `freebsd/COPYRIGHT` and
`THIRD_PARTY_LICENSES.md`. It is altered here (UTF-8 only, renamed symbols),
as `freebsd/PROVENANCE.md` lists.

## Exports

`compile`, `free`, `nsub`, `exec`, `matches`, `match_new`, `exec_into`,
`exec_range`, `match_count`, `match_start`, `match_end`, `match_free`, and the
flags `BASIC`, `EXTENDED`, `ICASE`, `NOSUB`, `NEWLINE`, `NOSPEC`, `NOTBOL`,
`NOTEOL`.
