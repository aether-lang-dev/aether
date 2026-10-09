- **A tuple return type takes pointer elements: `-> (*Foo, *Foo)` parses.**
  The parser told a `-> (T1, T2) { ... }` return type from a parenthesised
  `-> (expr)` body with a one-token check of its own (a type keyword or a
  name, then a comma), so a first element spelled with more tokens (`*Foo`,
  `mod.Name`, `fn(int) -> int`, `int?`) fell to the expression path and the
  function broke at top level. The type parser decides now, so a caller gets
  its pointers back typed instead of casting `ptr`s (#2626).
- **A pointer to one type passed where a parameter takes a pointer to
  another is a type error at the call.** `buffer_size(b: *Buffer)` given
  `&p.ints`, an `*Ints`, passed `ae check` and failed in gcc with
  "incompatible pointer type"; had the two structs shared a layout prefix,
  nothing would have failed and the callee would have read the wrong struct.
  It is E0200 naming both pointer types now, for `&local`, `&p.field`,
  `&p.a.b` and typed locals, while a bare `ptr`, the same struct through a
  module-qualified name and a `@c_struct` overlay pointer stay legal (#2624).
- **A module-level `var` of a function pointer type takes a function of that
  type.** `var g_free: fn(ptr, int) = noop_free` was refused with E0200,
  because the function was read as a closure value whose missing result did
  not match the annotation's `void`; `-> void` was a struct named `void` to
  the checker, so neither spelling helped. A global whose function returns a
  value passed the checker and then failed in gcc. A named function bound to
  a typed function pointer is now its address (through the #2586 adapter for
  a string result), `-> void` is the omitted return type, and module-level
  vars are emitted after the function prototypes (#2623).
- **Two locals of one name bound to different struct or pointer types in
  sibling loop bodies or `if`/`else` arms are refused by the type checker.**
  Such a local is one C variable, declared before the loop or the `if`, but
  the checker left sibling bindings to codegen, whose check only compared
  kinds, so `view = p as *Apple` in one `while` and `view = p as *Pear` in
  the next reached gcc as an assignment to `Apple*`. It is now the E0200 an
  int beside a string already was, and `ae check` reports it; a re-bind in
  one block names `*Apple` and `*Pear` instead of calling both `ptr` (#2611).
