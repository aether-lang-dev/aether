- **A module-level `const` of a function type holds the function.**
  `const CB: fn(int) -> string = label` failed in gcc: the const was read as
  a closure and emitted before the function prototypes and adapters, and
  `CB(i)` called a C function named `CB`. A typed fn-pointer const now holds
  the function's address, as a `var` does, is emitted after the prototypes,
  and is called through its type, a module's (`cbmod.CB(21)`) too. An
  unannotated `const CB = label` is a closure emitted after its adapter
  (#2648).
- **A module-level var or const can name an imported module's function.**
  `var g_name: fn(string) -> string = strfns.lit` was refused "module
  'strfns' has no export 'lit'", and later failed in gcc with `strfns_lit`
  undeclared: the prune that drops a module's unreached functions never
  looked at module-level initializers. They seed it now (#2650).
- **A typed fn pointer's own fn-pointer parameter is spelled out in C.**
  `meta(r: fn(fn(string) -> string, string) -> string, s)` called as
  `meta(relay, w)` failed in gcc with incompatible pointer types: the inner
  pointer was spelled `void*` in the declarator, struct fields and call
  casts, while `relay` is defined with the real pointer type. All of them
  share one spelling now (#2651).
- **A tuple type can have an optional element.** `-> (string?, int)`, from
  an Aether function or an extern, failed in gcc with "unknown type name
  'ae_opt_string'": the tuple typedef came before the optional's. A tuple
  typedef declares its elements' typedefs first, and a value returned in an
  optional slot is wrapped as a `-> T?` return wraps it (#2652).
- **A closure kept in a struct field is called directly.** `h.cb(2)` for a
  `cb: fn` field was "Undefined function 'h.cb'"; it is now the same call as
  `call(h.cb, 2)`, on a value or through a pointer (#2653).
- **A second receive arm for a message the actor already receives is a type
  error.** Two `Job(n) -> ...` arms in one actor compiled to two definitions
  of the handler, which gcc refused. The later arm can never run, so the
  checker refuses it and names the arm that already receives the message
  (#2654).
