- **A closure return of a number, where the closure returns a pointer, a
  struct or a closure, is an error.** A closure's result is held in its
  resolved C type, but what the return hands the caller (the retain, a
  struct or sequence copy) is chosen from the value's own type. A returned
  closure or struct the type checker had left typed `int` would have crossed
  without its reference; such a return now stops the build and says why.
  Numbers converting to numbers, and a closure returning a string on one
  path and `null` on another, compile as before.
