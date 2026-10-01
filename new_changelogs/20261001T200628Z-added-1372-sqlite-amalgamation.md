- **contrib.sqlite carries its own SQLite, natively and when cross-compiling.**
  The SQLite amalgamation (3.53.4) is pinned by version and SHA-256 in
  `contrib/sqlite/amalgamation.lock` and fetched on demand by
  `scripts/fetch-sqlite-amalgamation.sh`; it is not committed. `make contrib`
  compiles it into `build/contrib/libsqlite3.a` beside the veneer, so a
  program using contrib.sqlite needs no `libsqlite3` where it runs, and
  `ae build --target=<triple>` compiles it for the target, so
  `--target=aarch64-linux-musl` links a working SQLite with nothing staged.
  The ~1 minute amalgamation compile is cached: per target in the ae cache,
  and in CI. The system `libsqlite3` remains the fallback when the
  amalgamation cannot be fetched, and release archives ship it (#1372).
