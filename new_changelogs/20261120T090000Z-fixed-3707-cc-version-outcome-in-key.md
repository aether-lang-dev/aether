- **An unchanged rebuild hits the cache even when the C compiler's
  `--version` failed once (#3707).** The build cache key folds in the first
  line of `<cc> --version`, but not how that run ended: a spawn failure, a
  signal or an exit with nothing on stdout folded in no line, so that build
  published under another key and the next identical build missed (seen on
  macOS x86, where the run goes through the xcrun shim). Such a run is now
  tried again, up to three times, and its outcome is part of the key, so a
  compiler that always rejects `--version` (tcc) still caches. A compiler
  whose `--version` never runs to an answer leaves the build uncached rather
  than keyed without it, and `ae build -v` says so.
