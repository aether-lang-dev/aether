- **`release_contrib_resolves` builds against the prebuilt runtime.** Its
  hand-built release layout left out `lib/libaether.a`, which the archive
  ships, so each of its two builds compiled the whole runtime from source:
  about four minutes of every Windows CI job. It copies the library in now,
  and takes 10 s locally instead of 97 s (#2596).
