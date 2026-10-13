- **`ae` builds every path and name whole, or refuses it.** gcc 15 and 16
  at `-O2 -Wall -Wformat-truncation=1` found fixed buffers in `tools/ae.c`
  that a long value could overflow, and `ae` then went on with the cut
  value: the shared runtime's link argument, the toolchain root taken from
  `AETHER_HOME` or the working directory (and every probe below it), a
  dependency's declared module path, a package's module names (and the
  `--package` name), a namespace's function list entries, script paths and
  import lines, a project directory's `src/main.ae`, the `.wasm` path (two
  bytes past its buffer), an example's path, the Windows abnormal-exit note
  (always cut) and `AETHERC` for `ae help`. They are heap paths now,
  writes that report an overflow, or buffers sized for the longest value; a
  root too long for the toolchain, or a package name past 255 bytes, is an
  error. `tools/ae.c` compiles with no such warning on either compiler
  (#2862).
