- **`ae` removes the stub directory of a binary import when it exits.** A
  build that imports a binary package writes the import's interface stub
  into a fresh `ae-binimport-*` temp directory, read only by the compile
  that follows, and left it there: every such build added one for good (889
  on one Windows CI machine). It is removed when `ae` exits, and on Linux and
  macOS it is made under `TMPDIR` as other temp files are, rather than
  always under `/tmp` (#2620).
