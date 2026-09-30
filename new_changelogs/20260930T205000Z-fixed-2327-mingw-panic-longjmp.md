- **A caught panic no longer crashes on Windows (mingw-w64 x64).** The
  panic's longjmp used mingw's default `setjmp`, which makes msvcrt's
  `longjmp` a structured-exception unwind through every frame back to the
  `try`; that unwind faulted intermittently inside `RtlVirtualUnwind2`, so a
  panic caught by `try` crashed the process (`std/sort/test_sort.ae`: 28 of
  200 parallel runs under MINGW64). The panic now uses `_setjmp(buf, NULL)`,
  mingw's non-unwinding form, the same register restore POSIX gets from
  `_setjmp`/`_longjmp`; cleanup stays the allocation journal's (#2327).
