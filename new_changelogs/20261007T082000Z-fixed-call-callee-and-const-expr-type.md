- **`call()` on a value that is not a closure is a type error (#2468).**
  `call(x, ...)` invokes a closure, and a callee of any other known type was
  let through to the C compiler, which stopped at `'_tuple_ptr_string' has
  no member named 'fn'` against generated code. The usual way in was a
  `(value, err)` return such as `list.get` bound to one name. The checker now
  reports `call() needs a closure, but 'cl' has type (ptr, string)` at the
  argument, with how to destructure the tuple, or to unbox a closure stored
  as a `ptr`. The closures-and-builder-DSL guide read list elements with
  `list.get` where it needed `list.get_raw`, and now uses `list.get_raw`.
- **A const defined by an operator expression has its type everywhere it is
  read (#2475).** A const was typed at registration only when its
  initializer was a bare literal, so `const MOVED = 1 << 30` stayed untyped
  until the checker reached the declaration. An imported const is merged at
  the end of the program and a same-file const can come after its users, so
  `mark = n.flag_index & flags.MOVED` warned "unresolved type in codegen,
  defaulting to int", and with a 64-bit const the int silently truncated the
  value (`v | HIGH` with `HIGH = 0x100000000 << 2` gave `v`). A const is now
  typed from its initializer's operators (`<<`, `>>`, `&`, `|`, `^`, `~` and
  the arithmetic ones) and from the other consts it names, in its own module
  or another, before any function is checked.
