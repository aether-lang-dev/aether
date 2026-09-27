- **A build without zlib links again.** `std/zlib/aether_zlib.c`'s
  `!AETHER_HAS_ZLIB` branch was missing a stub for `zlib_try_deflate_raw` (the
  function `std.zip`'s writer calls for raw deflate), so any program using
  `std.zip`, or the release archive-export check itself, failed to link
  without zlib present. `make ci` now also builds a `ZLIB=0` stdlib archive
  and runs the export check against it, so a std module missing a no-zlib
  stub fails on every CI run instead of only the release's FreeBSD leg
  (#2207).
