- **std.posix.regex: POSIX regular expressions beside PCRE2 (#3652).** Basic
  (the default) and Extended syntax, POSIX leftmost-longest matching and
  back-references, from Henry Spencer's engine as FreeBSD's libc carries it,
  vendored in `std/posix/regex/freebsd/` and built into libaether.a. std.regex
  (PCRE2, leftmost-first) cannot stand in for it: `a|ab` on "abcd" is "ab"
  under POSIX and "a" under PCRE2, so a sed or grep built on PCRE2 gives
  different output for real scripts, and BRE syntax (`\(`, `\{m,n\}`, literal
  `+ ? |`) has no PCRE2 spelling. The engine is the same on every target,
  Windows included, where there is no `regcomp` at all; it is trimmed to
  UTF-8 only (no locale or collation tables, so a pattern means the same
  whatever `setlocale` says) and its symbols are prefixed `aether_posix_*`, so
  it cannot clash with a host libc's `regcomp`. `regex.compile(pattern,
  flags) -> (re, err)` takes `EXTENDED`, `ICASE`, `NOSUB`, `NEWLINE` and
  FreeBSD's `NOSPEC`, with regerror's text as `err`; `regex.exec(re, subject,
  eflags)` (`NOTBOL`, `NOTEOL`) returns a caller-owned match of submatch
  offsets, and `match_new` / `exec_into` / `exec_range` reuse one buffer and
  match a byte range (`REG_STARTEND`). No module or thread-local state.
  FreeBSD's regex regression data (507 cases) and the AT&T conformance data
  it runs (394 cases, its known-bug list included) pass; a POSIX case table
  agrees with the system `regcomp` on FreeBSD and macOS. libaether.a grows by
  about 52 KB.
