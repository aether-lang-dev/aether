- **A `longdouble` interpolates and prints with `.` whatever the process
  locale, as a float now does (#3525).** `"${x}"` of a `longdouble` used
  plain `snprintf("%Lg")` and `print(x)` emitted `printf("%Lf")`, so under a
  comma-decimal locale they wrote `0,5`. The runtime's locale independent
  formatter takes a long double too, and both go through it.
