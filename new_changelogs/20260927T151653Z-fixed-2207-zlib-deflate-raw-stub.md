- **A build without zlib links again, and releases publish again.**
  `std/zlib/aether_zlib.c`'s `!AETHER_HAS_ZLIB` branch was missing a stub for
  `zlib_try_deflate_raw` (the function `std.zip`'s writer calls for raw
  deflate), so any build without zlib failed to link a program that uses it.
  The release's FreeBSD cross-build has no zlib, so its archive export check
  failed on every release from 0.714.0 to 0.731.0 and none of them published.
  `make ci` now also runs the export check against a copy of the archive whose
  zlib-dependent objects are compiled without zlib, so a std module missing a
  no-zlib stub fails on every CI run instead of only the release's FreeBSD
  leg (#2207).
