- **A closure stored in a typed function pointer is a type error wherever it
  is stored.** A typed fn pointer (`fn(ptr) -> string`, a `cfn` name) is a
  bare C function pointer and has no room for a closure's environment. Only
  an argument was refused; a closure, or a local bound to one, in a struct
  literal's field, a field or element store (through a pointer too), an
  annotated local, a re-bind of one, a module-level `var` or a `-> Getter`
  result passed `ae check` and failed in gcc against an `_AeClosure`. Each is
  E0200 now, naming the field or variable and the two spellings that work: a
  named function, which is stored as its address, or a bare `fn` slot called
  with `call(...)` (#2628).
