- **A variable named `stdout`, `stderr`, `errno`, `EOF` or another C header
  macro compiles.** The generated C includes `<stdio.h>`, `<stdlib.h>`,
  `<stdint.h>` and `<time.h>`, and the preprocessor rewrote any identifier
  spelled like one of their macros before the C compiler saw it: `EOF = 1`
  became `(-1) = 1`, `errno = 5` an assignment to a function call, and, under
  the Windows UCRT, `stdout = x` became `(__acrt_iob_func(1)) = x`. The
  program type-checked, and the C compile then failed with errors pointing
  into generated code. glibc defines `stdout` as itself, so the most natural
  case, `stdout, stderr, code, err = os.run_full(...)`, built on Linux and
  failed only on Windows. The stdlib guide carried a warning telling readers
  not to use those names.

  Codegen now renames these names the way it already renamed C keywords and
  Windows SDK names, in locals, parameters, tuple targets and struct fields,
  on every platform so a program behaves the same everywhere. The list is the
  object-like macros of the headers the generated C includes (`stdin`,
  `stdout`, `stderr`, `EOF`, `BUFSIZ`, `SEEK_*`, `EXIT_*`, `RAND_MAX`,
  `errno`, `CLOCKS_PER_SEC`, the `<stdint.h>` limits), plus `environ`. A
  name the program imports on purpose with `extern const NAME: T @c_import`
  keeps its C spelling, since it *is* the macro.
  `tests/regression/test_c_header_macro_idents.ae` covers each position and
  the `@c_import` exemption.
