- **The compiler's driver keeps every name and list it is given (#3574).**
  A `-D` symbol of 128 characters or more was refused as "too many -D
  symbols"; past nine `--binimport-stub-dir` directories the module of the
  ninth was gated as the program's own, so its externs were refused under
  `--emit=lib`; `--emit=inspect` cut a function's tuple return type at 256
  bytes; a compiled module artifact recording a build symbol or source path
  longer than 511 characters was refused as malformed every time it was
  read, and a module asking about more than 128 build symbols got none; and
  a diagnostic's path deeper than 256 directories, or longer than 8192
  bytes, was shown cut. Each now grows with what it holds.
