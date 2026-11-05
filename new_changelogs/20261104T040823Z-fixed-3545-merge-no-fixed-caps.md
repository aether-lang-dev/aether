- **The module merge reads everything past the limits it used to stop at
  without a word (#3545).** A merged function's locals were read into 128
  slots, so a local past them named like a module function was taken for the
  function; a module's aliased imports into 64, so `a.f()` past the 64th kept
  a namespace nothing binds; a selective import's names into 256 for the
  check that a local function does not shadow one, so a shadowing function
  past them was not refused; and names were joined into 256-byte buffers, so
  two long names that differ past that merged under one C name. Every list
  now grows with what it lists and every name and path is as long as it is
  written.
