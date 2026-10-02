- **Releases package again, and a pull request now shows a packaging break.**
  0.757.0, 0.758.0 and 0.759.0 never published their archives:
  - 0.757.0 failed on every platform. The release builds its tag, which was
    cut before #2344 merged, with the workflow file from main's HEAD. That
    workflow had a step calling a script the tag did not have.
  - 0.758.0 and 0.759.0 failed on Windows, whose release image had neither
    `unzip` nor `python3` to extract the SQLite amalgamation.

  What a release archive holds is now defined once, in
  `scripts/stage-release.sh`. The Unix, Windows and FreeBSD legs of
  `release.yml` call it. So does `make test-release-archive`, which before
  rebuilt the layout by hand and had drifted from the real one (a different
  `libaether.a` path, no contrib, no SQLite). CI runs that test on Linux,
  macOS and Windows, so the next packaging break fails a pull request
  instead of a release. The archive's headers are now every header under
  `runtime/` and `std/`, as `install.sh` installs them, where the release
  listed directories by hand and missed newer ones (`std/udp`, `std/xml`).
