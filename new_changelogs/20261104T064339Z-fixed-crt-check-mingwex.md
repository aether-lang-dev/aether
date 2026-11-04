- **The Windows CRT check no longer fails the release on mingw-w64's own
  `ldexp`.** From mingw-w64-crt 14.0.0.r598, `ldexp` is gone from
  `libucrt.a` and lives only in `libmingwex.a`, which every MinGW link
  takes, while `libmsvcrt.a` keeps an alias for it. `windows_crt_symbols`
  read that as a function only msvcrt has, and failed the Windows build of
  the v0.876.0 and v0.877.0 releases, so neither was published. A symbol
  `libmingwex.a` implements is now counted as CRT-independent, as the
  check's own comment already said; a real msvcrt-only call still fails it.
