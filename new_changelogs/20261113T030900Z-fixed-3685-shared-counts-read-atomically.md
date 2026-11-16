- **Strings and slices: a shared count is read atomically everywhere (#3685).** A
  string or slice buffer reachable from several threads has an atomic count,
  marked by a bit in it, but the code that decides between the atomic and
  the plain path (retain, release, the check that a pointer is a string,
  the generated release and hand-over helpers, a slice's grow in place) read
  the count with a plain load while other threads changed it atomically.
  ThreadSanitizer reported it in five programs (`make test-tsan`, #3542). The
  count is read with an atomic load now; a slice grown in place, which
  needs the count to be one, reads it with acquire.
