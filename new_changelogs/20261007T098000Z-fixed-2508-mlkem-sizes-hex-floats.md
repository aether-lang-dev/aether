- **ML-KEM's size helpers answer 0 for a `k` that is not a parameter set,
  and hex floats parse on Windows with msvcrt (#2508).**
  `mlkem_ek_bytes(k)`, `mlkem_dk_bytes(k)` and `mlkem_ct_bytes(k)` built a
  heap parameter bundle for any `k` and read fields only `k` = 2, 3 and 4
  set, so another `k` returned whatever the allocator left there; the
  `malloc` result was not checked either. The bundle is now a plain value,
  with no allocation, and the helpers return 0 for any `k` other than 2, 3
  or 4. `string.to_double("0x1p3")` gave 8 with glibc and UCRT but an error
  with msvcrt, whose `_strtod_l` reads no hexadecimal constants. `to_double`
  and `to_float` now read `[+-]0x<hex>[.<hex>][p[+-]<dec>]` themselves on
  every platform, rounded to nearest (ties to even) straight to the result
  type, subnormals included; a value past the type's range is an error, as
  for decimal text.
