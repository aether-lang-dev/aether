- **`std.cryptography.mlkem` runs the FIPS 203 input checks and returns an
  error instead of a key (#2482).** Encaps and decaps checked no length and
  no range: a ciphertext with extra bytes decapsulated to the real secret, a
  100-byte or empty one returned a key, an encapsulation key with a
  coefficient of q or more was used as is, a short decapsulation key or `m`
  was padded with 0xFF, and a short encapsulation key panicked. Encaps now
  runs the §7.2 check (length 384k + 32, every coefficient below q) and
  decaps the §7.3 checks (ciphertext length, decapsulation-key length and
  its H(ek) hash check), returning an error and null outputs when one fails.
  The API changes with it: `mlkemN_keygen() -> (ek, dk, err)` and
  `mlkemN_encaps(ek) -> (ct, key, err)` draw their randomness from the OS
  CSPRNG, and `mlkemN_decaps(dk, ct) -> (key, err)`. The caller-seeded forms
  are `mlkemN_keygen_derand(d, z)` and `mlkemN_encaps_derand(ek, m)`, for
  known-answer tests only (FIPS 203 §3.3), with the same checks plus 32-byte
  `d`, `z` and `m`. `s16` is branch-free, since it runs on secret
  coefficients. The Wycheproof driver now asserts the error on every invalid
  case (it used to count any output that differed from the empty expected
  key as a rejection) and also sweeps the encapsulation and semi-expanded
  decapsulation vectors.
- **`string.to_int_radix` accepts only what its documentation says, and
  `to_double` / `to_float` read `inf` and `nan` on Windows (#2472).**
  `to_int_radix("0x10", 16)` returned 16, and a leading space or `+` was
  accepted too, because the parse was `strtoll`. It now accepts exactly an
  optional `-`, digits of the radix and optional trailing whitespace, with
  the int64 bounds exact. On Windows, msvcrt's `_strtod_l` does not read
  `inf`, `infinity` or `nan`, so the `Infinity`, `-Infinity` and `NaN` that
  `from_double` writes did not read back. These spellings (any case, signed,
  and `nan(...)`) are now recognised before the platform parser runs, the
  same way on every platform.
