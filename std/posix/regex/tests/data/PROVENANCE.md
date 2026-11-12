# Regex test data

Copied unchanged from FreeBSD's tree at commit
`9d2cdb3fd5741685622b0b9c0e17e22583c9ceff`, the same commit as the engine
(`../../freebsd/PROVENANCE.md`):

- `*.in`, `README`: `contrib/netbsd-tests/lib/libc/regex/data/` and its
  `README` (the file format). Henry Spencer's regex test set, split into files
  by NetBSD; FreeBSD runs it as `lib/libc/tests/regex` (`h_regex` built from
  NetBSD's `main.c` and `split.c`). `../../test_posix_regex_freebsd.ae` is that
  driver ported to Aether.
- `att/*.dat`, `att/README`: `contrib/netbsd-tests/lib/libc/regex/data/att/`,
  Glenn Fowler's AT&T Research regex conformance data ("license free", per
  `att/README`). FreeBSD runs it with NetBSD's `t_regex_att.c` built
  `-DREGEX_SPENCER -DSKIP_LEFTASSOC`; `../../test_posix_regex_att.ae` is that
  driver ported to Aether, known-bug tables included.
