- **Compile time grows linearly with a program's size again (#3499).**
  Three lookups by name scanned the whole program for each call or each
  top-level node: the checker's callee and extern lookups, codegen's
  function-pointer globals and its set of function names that a local may
  shadow, and the merge's import prefixes. A chain of 8000 functions took
  1 s here and 22 to 25 s on the release's macOS x86_64 runner, which
  failed the release twice. Each lookup is now answered from an index
  (the checker's clause index, codegen's program index, a hashed name set),
  and the same chain compiles in a third of a second; at 32000 functions,
  1.3 s instead of about 20. `order_effects_scaling` now compares 4000 and
  16000 functions on the same runner, so it no longer depends on the
  runner's speed.
