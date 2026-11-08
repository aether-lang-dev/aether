- **The FreeBSD release has a working `std.regex`.** Its cross build
  (`make FREEBSD=1`, which the release job runs) set `PCRE2 := 0`, so the
  release's `libaether.a` carried the regex stub, and every `regex.compile`
  failed with "regex: built without libpcre2-8". It now builds the vendored
  engine, as the Windows and Android cross builds already did, and the release
  and PR cross-build jobs check the archive has it
  (`scripts/check_regex_engine.sh`).
