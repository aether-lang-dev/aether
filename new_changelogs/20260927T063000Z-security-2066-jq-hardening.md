- **`contrib.jq` withstands hostile programs and input.** Nesting past the
  stack, in a program (`((((…))))`, `1,1,1,…`, nested `\(…)`) or in a value
  a program builds, now ends in a jq error instead of a crash, through one
  recursion guard shared by the lexer, parser and evaluator and a 512-level
  cap on built values. Indexes and counts taken from numbers saturate
  instead of overflowing (`.[1e300:]` was the whole array, `limit(1e300; f)`
  was nothing, `flatten(1e300)` did not flatten); `.[1e9] = 1` raises
  `Array index too large` and `"ab" * 1e300` `Repeat string result too
  long`, as in jq; `range(0; 10; nan)` ends. `\u0000` is refused rather
  than silently truncating a string (which made `{"a\u0000b":1,"a":2}`
  two `"a"` keys), a repeated input key resolves to its last value
  everywhere, `getpath` no longer copies at every step (`tostream` on a
  256-deep document took ten seconds) and global regex matching is linear in
  the subject. `test_nesting.ae`, `test_depth.ae` and `test_hardening.ae` hold each of these
  (#2066).
