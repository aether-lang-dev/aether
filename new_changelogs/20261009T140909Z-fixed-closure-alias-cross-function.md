- **A function that only aliases a closure parameter keeps nothing of its
  caller's.** `wrap(cb) { a = cb; call(a) }`: the alias takes a reference
  of its own, but the caller counted it as a keep of its argument, so
  neither a closure local nor a closure literal it passed was ever
  released, one environment per call (#2670).
- **A function returning an alias of a closure hands its caller that
  reference.** `make() -> fn { b = || { ... }; a = b; return a }` returned
  the alias's own reference, but its callers did not count the alias as a
  closure handed over, so they never released what they got (#2671).
