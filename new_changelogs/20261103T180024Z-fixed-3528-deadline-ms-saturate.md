- **A deadline too far off to count in nanoseconds no longer stops the work
  at once (#3528).** The runtime turned a millisecond deadline into
  nanoseconds with `ms * 1000000LL`, which overflows past about 292 years,
  so a host passing `long.max` to mean "no deadline" got a deadline in the
  past, and an ACTIVE budget came out negative. The process deadline, a
  scoped deadline, and an execution account's budget and deadline now
  saturate, and a deadline past the end of time never trips.
