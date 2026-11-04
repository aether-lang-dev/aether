- **A closure a binary-imported library returns is given back (#3547).**
  An Aether library's export hands its caller the reference its return
  gives, but the program's stub declared the export as a plain extern, and
  a closure an extern returns is lent: the program retained it once more,
  released only its own reference, and the library's environment leaked on
  every call. An extern now takes `-> fn @heap` (as `-> string @heap`),
  which says the caller owns the closure, and the stub of a binary import
  declares its closure results so.
