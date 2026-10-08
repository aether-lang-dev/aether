- **A function named like a C math or character function no longer
  replaces it (#2526).** A top-level `floor(x: int) -> int` was emitted as
  a global `floor`, the link resolved libm's `floor` to it, and
  `math.floor(2.5)` called the user's function and returned 2.5; a
  `toupper` failed to link on Windows. The compiler already gave socket,
  I/O, process, memory and string names a C symbol of their own; it now
  does the same for every `<math.h>` function (with its `f` and `l`
  forms), `<ctype.h>`, `<setjmp.h>`, `<locale.h>` and the rest of the C11
  library. The Aether name is unchanged.
