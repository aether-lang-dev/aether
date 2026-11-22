- **`ae build --emit=obj` compiles the object as the executable is compiled
  (#3812).** A native `--emit=obj` ran the C compiler with the include paths
  and nothing else: no `-fwrapv`, so signed overflow was undefined in the
  object and defined in the executable; no optimisation level; and none of
  the project's `[build] cflags` or `AETHER_CFLAGS_EXTRA`, so a program that
  needs a define from them built as an executable and not as an object. The
  object's compile line now takes the flags from the same place as the
  executable's compile step (`-O2` or `-O0 -g`, `-fwrapv`, the hardening
  defines, the GCC inlining budget, the user's cflags, Clang's bracket
  depth). A cross `--emit=obj` already carried them.
