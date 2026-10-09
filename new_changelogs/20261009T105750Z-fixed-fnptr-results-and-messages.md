- **A message field typed as a C function pointer takes a named function.**
  `message Job { run: fn(int) -> int }` refused `Job { run: double_it }` with
  "expected closure, got int", and a handler's `run(21)` called the field as
  a bare `void*`, which gcc refused. The sender stores the function's
  address, as a struct field does, and the handler's binding is called
  through the field's signature, inside a closure in the arm too; a string
  result is the caller's, and a closure there is refused (#2633).
- **A call to a function returning a named function pointer type is that
  pointer.** `g = mk()` for `mk() -> Getter` was "Type mismatch in variable
  initialization": the early inference pass typed the call as what calling
  the pointer would give. It binds to a local, a typed `let`, a field and a
  parameter now, `f as ptr` gives back the pointer a typed fn pointer holds,
  and messages spell such a type `fn(ptr) -> string` instead of "closure"
  (#2634).
- **A call through a local's fn-pointer field counts as a use of the
  local.** `println(f.get_text(null))` as the only use of `f` warned W1001
  "unused variable 'f'": the call names `f.get_text` and has no identifier
  for the receiver. The usage walk reads the receiver from the call (#2635).
- **A return type can be a function pointer type written out:
  `-> fn(ptr) -> string`.** It broke at top level, read as an arrow body
  calling `fn`; the type parser decides now, the inner `-> R` belongs to the
  type, and `-> fn(int)` returns nothing. A fn pointer destructured from a
  tuple result is called through its type as well, where it was called as a
  bare `void*` (#2636).
