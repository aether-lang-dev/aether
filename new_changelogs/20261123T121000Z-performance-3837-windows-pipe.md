- **`ae` passes `-pipe` to the C compiler on Windows too (#3837).** Only the
  POSIX compile line had it, so on Windows each stage of compiling the
  generated C went through a temporary file. GCC, Clang and `zig cc` all
  take it, and the tree's own Windows make already used it. A cold
  `ae build` of hello is about 30 ms faster there.
