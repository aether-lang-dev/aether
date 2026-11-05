- **`ae cflags` prints `-fbracket-depth=2048` for a Clang (#3574).** A
  build that compiles `aetherc` output itself with the compiler `ae`
  drives (`$AE_CC`, `$CC`, else `gcc`, else `cc`) now gets the bracket
  depth every `ae` build gives Clang, with the include flags, when that
  compiler takes the option; GCC, which refuses it, does not.
