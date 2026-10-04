- **`contrib.quickjs`: modern JavaScript, embedded.** quickjs-ng (ES2023:
  classes, async/await, `Map`/`Set`, `?.`, `??`, modules) compiled into the
  program from its pinned amalgamation (`contrib/quickjs/amalgamation.lock`,
  fetched by `scripts/fetch-quickjs-amalgamation.sh`; release archives ship
  it), natively or with `ae build --target`. Values cross as handles, never
  as QuickJS's 16-byte `JSValue`; host functions are Aether closures, all
  served by one C dispatcher; each entry into JS has a time limit, and the
  runtime a memory cap. A script awaits promises Aether settles later, so
  work on an actor can answer it. quickjs-libc is never compiled in, so a
  script reaches only the host functions it is given. Tested on macOS,
  Linux x86_64 and arm64, FreeBSD, and Windows (under Wine); leak-clean
  under valgrind.
