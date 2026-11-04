- **Float interpolation and `print` write `.` whatever the process locale
  (#3525).** `"${x}"` formatted a float with plain `snprintf` and `print(x)`
  emitted `printf("%f")`, so in a host that had called
  `setlocale(LC_ALL, "de_DE.UTF-8")` they wrote `0,5` and `0,500000` while
  `string.from_float(0.5)` wrote `0.5`. Interpolation now formats a float
  through the locale independent formatter `string.from_float` uses, and
  `print` and `println` of a float, or with a format string, go through the
  same runtime formatter as an interpolation. Output in the C locale is
  unchanged.
