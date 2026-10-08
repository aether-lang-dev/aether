- **String interpolation and an interpolated `print` are faster.** The
  runtime formats an interpolation once, into a stack buffer for any result
  up to 256 bytes, and writes integer segments itself instead of through
  `snprintf`; floats keep printf's `%g` text. On Windows (MinGW), two
  million `"id=${i} name=${name} x=${i * 3}"` take about 180 ms against 310
  ms with the previous `vsnprintf` path, and a million such `println` lines
  about 190 ms against 415.
