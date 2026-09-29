- **A `print` of an interpolated string flushes stdout like any other
  `print` (#2278).** It wrote through `printf` and skipped the flush, so its
  text sat in the buffer when stdout was a file or pipe. A process killed
  or ended with `_exit` lost it. `std.spec` prints its result lines this
  way, so a spec file killed by a timeout lost the line for the last spec
  that passed.
