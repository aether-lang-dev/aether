- **`ae run` and `ae build` ask the C compiler for its `--version` while
  they do other work (#3842).** The answer is part of every cache key, and
  the run (a process start and a temporary file, 10 to 12 ms on Windows)
  was waited for before the key walked the headers and folded the
  depfile. It now starts where the command begins and is waited for where
  the key is put together. The answer is the same, asked on every key as
  before and never remembered by the compiler's file identity: only the
  waiting overlaps. A warm-cache `ae run` is about 10 ms faster on Windows.
