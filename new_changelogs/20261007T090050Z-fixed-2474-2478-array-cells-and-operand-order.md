- **A closure can write into a fixed-size array it captures, and operands
  are evaluated left to right (#2474, #2478).** A write to a captured
  `int[3]` (`arr[i] = v`, `arr[i]++`, `arr = [...]`) was refused at compile
  time, because the shared cell a written capture lives in was spelled
  `T* name` and `int[3]*` is no C declarator. The cell is now a pointer to
  the whole array, so the closure and the enclosing function see one array,
  through nested closures, loops and calls that take it as a slice. A
  string array's cell owns its elements: a store frees the element it
  replaces and takes a buffer of its own, and the last release frees them
  all. A closure in an actor handler that writes an element of a state
  array is refused like any other write to state. Separately, a call's
  arguments, an interpolation's segments, the two sides of `+`, the fields
  of a struct literal or a message, an array literal and a multi-value
  `return` reached C in a form that leaves their order unspecified, so
  `"${j++} ${j++} ${j}"` printed `1 0 2` with GCC on Windows and
  `add(next(), counter)` read the global before the call that changes it.
  An operand a later one depends on (one writes a variable the other uses,
  or a call can change what the other reads) is now evaluated first, in
  source order; lists with no such pair compile as before. An array literal
  stored into an existing array is evaluated whole before the store, so
  `a = [a[1], a[0]]` swaps.
