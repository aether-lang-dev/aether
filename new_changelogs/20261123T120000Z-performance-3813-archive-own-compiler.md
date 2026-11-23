- **A cold build by the archive's own compiler no longer asks it which C
  runtime it targets (#3813).** `libaether.crt` now also names the compiler
  that built `libaether.a` (`cc <id>`, the identity the build cache keys
  on, which the new `ae cache ccid` prints), written only when that
  compiler given no flags targets the recorded runtime. A build by the same
  compiler with no cflags links the archive without the probe compile:
  one compiler start less on every cold cache, 8 commands to 7 for
  `ae build -v benchmarks/core/compile_heavy.ae`, 491 to 465 ms on Windows
  (10 runs each). Any other compiler, any cflags, and a record without the
  line are asked as before.
