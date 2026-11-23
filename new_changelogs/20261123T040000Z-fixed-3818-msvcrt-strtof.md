- **`string.to_float` on msvcrt links with the msvcrt WinLibs and rounds
  correctly (#3818).** The runtime parsed floats with `_strtof_l`, which
  msvcrt.dll does not export. The msvcrt WinLibs GCC 14.2 (mingw-w64 12)
  has no import for it, so with it as `AE_CC` a program did not link
  ("undefined reference to `__imp__strtof_l`"). MSYS2's MINGW64 (mingw-w64
  15) links one that narrows `_strtod_l`, which rounds twice: text just
  above a float tie came out one ulp low ("1.00000005960464477540" gave
  1.0, not 1 + 2^-23), text just under `FLT_MAX` plus half an ulp
  overflowed, and text just above 2^-150 gave 0. On msvcrt the runtime now
  parses with mingw-w64's correctly rounded parser, under a comma-decimal
  locale too, and gets UCRT's bits. UCRT and MSVC builds are unchanged.
