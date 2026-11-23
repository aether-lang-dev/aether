- **A closure return whose value is a different kind of value than the
  closure returns is an error.** A closure's result is held in its resolved
  C type, but what the return hands the caller (the retain, a struct or
  sequence copy) is chosen from the value's own type. Where the two
  disagreed, a returned closure or struct the type checker had left typed
  `int` would have crossed without its reference. Such a return now stops
  the build and says why. Number-to-number returns convert as before.
