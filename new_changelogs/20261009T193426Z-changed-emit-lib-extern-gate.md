- **A capability-empty `--emit=lib` build rejects the program's own
  `extern`s.** The `--with=fs,net,os` gates stopped `import std.os`, but a
  library could still declare `extern system(cmd: string) -> int` (or
  `@extern("fopen") …`, or put either in a local module) and call it, so the
  gates were decorative against anyone writing one line of C binding. Such an
  extern is now an error that names it, unless the build passes the new
  `--with=extern`. `--with=all` / `first-party` include it. Externs declared by
  `std.*` and `contrib.*` modules are unaffected; those modules sit behind the
  other gates. `ae inspect` reports `extern` among the capabilities a file
  needs, and a `--emit=csrc` catalog lists it when granted. The same applies to
  `--emit=csrc` and `--emit=obj`, which are library builds too. **Breaking** for
  a library that declares its own externs: build it with `--with=extern`
  (downstream: check `ae build --emit=lib` invocations in servirtium-vcr, aeb,
  aether-ui and any binding that ships its own C shim).
