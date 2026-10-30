- **The cross-version ABI matrix expects ELF's merged runtimes.** On Linux and
  FreeBSD a library with a runtime of its own binds to its importer's runtime,
  so an Aether consumer of a library from another release is refused by the
  library's ABI check, with its reason; the matrix failed on every release
  since 0.860.0 by expecting the pair to run.
