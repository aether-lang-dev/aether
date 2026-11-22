- **The build cache knows a C compiler by the command as given (#3814).** The
  cache key folds in the first line of the C compiler's `--version` (#2477),
  but asked it of each program named in `$AE_CC` / `$CC` alone: `zig cc`
  asked `zig --version`, which every zig refuses alike, and the `cc` word was
  looked up on PATH and asked of whatever compiler that was (MinGW GCC's
  `cc.exe` on Windows). A zig upgrade was served the old one's binaries. The
  version is now asked of the whole command, `zig cc --version`, which names
  zig's clang.
