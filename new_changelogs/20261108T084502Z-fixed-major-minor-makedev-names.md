- **A function named `major`, `minor` or `makedev` compiles on FreeBSD.**
  FreeBSD's `<sys/types.h>` (and glibc's before 2.28) defines all three as
  function-like macros, so the generated C for a user `major(seg: string)`
  became `__major(seg)` and failed against `dev_t`. They join the C names
  codegen renames (`ae_major`). Test:
  `tests/regression/test_string_local_returned_as_ptr.ae`.
