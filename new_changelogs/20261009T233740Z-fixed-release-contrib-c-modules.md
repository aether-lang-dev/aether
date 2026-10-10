- **Every C-backed contrib module builds from a binary release.** A release
  ships contrib's C as source and builds no `libaether_<x>.a` archives, so on
  a release-only install `import contrib.sqlite` failed to link
  (`library 'aether_sqlite' not found`), and contrib.tinyweb,
  contrib.i18n.collate and contrib.avcodec failed on their missing C. With
  no `libaether_sqlite.a` beside `libaether.a`, `ae build` now compiles the
  veneer and the pinned amalgamation the release ships, caches both objects
  and links them in place of `-laether_sqlite -lsqlite3`, as the cross build
  already did; tinyweb (`ws_handshake.c`), i18n.collate (`aether_i18n.c`,
  `ducet_data.c`) and avcodec (`aether_avcodec.c`, plus FFmpeg's libraries
  by `@link`) name their C with `@source`, so it ships and compiles in.
  On FreeBSD a native build now searches `/usr/local/include` and
  `/usr/local/lib` (or `$LOCALBASE`) as it does Homebrew's on macOS, so a
  module's C can reach a package's headers (FFmpeg's, for avcodec).
  `tests/integration/release_contrib_c_modules` builds and runs each from a
  tree `scripts/stage-release.sh` staged.
