- **`std.json` writes a double so that parsing it gives back the same double
  (#2290).** `stringify` wrote numbers with `%g`, six significant digits, so
  `0.061234567891` came back as `0.0612346` and `3.141592653589793` as
  `3.14159`. It now writes `%.15g` when that reads back as the same double,
  else `%.16g`, else `%.17g`, which always does. A value that needs few
  digits still gets few (`0.1` is `0.1`), and a number below 1e15 stays in
  fixed notation (`1e10` is written `10000000000`). A NaN or an infinity,
  which JSON cannot spell, is written as `null` instead of the invalid `nan`
  or `inf`.
