- **A trust anchor's own pathLenConstraint is pinned by a test (#3432).**
  Path validation has checked the anchor as it checks every CA since the
  rewrite for #3303, so a root with pathLenConstraint 0 allows no CA below
  it, as on OpenSSL and Go. Nothing tested it: crypto_tls13_chain now has a
  real root with pathLenConstraint 0, refused with a CA below it and taken
  with an end entity directly under it, and the anchor check, taken out of
  the path loop, fails the first case.
