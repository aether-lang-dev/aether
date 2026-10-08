- **A string field replaced through a pointer parameter frees the value it
  replaces (#2565).** A store through `p: *T` never freed the old string,
  because the box's ownership tracker is only safe to read on a `heap.new`
  box (#1873), and a parameter was always of unknown origin: every helper
  written as `set(p: *T, ...)` leaked each string it replaced. A parameter
  now holds what its callers pass. Where the compiler sees every call (a
  function of an imported module, or a file-local `name_`, that is never
  used as a value, a `@c_callback` or a builder, outside a library build)
  and each passes a `heap.new` box, or a parameter that holds one, through
  any chain of helpers and recursion, the store frees the old value. A
  function with any other caller, or a top-level function of the entry
  file, which C code built into the program may call, keeps the old
  behaviour.
- **A string field handed to a function that frees it is not freed again
  (#2566).** `p.name = set_owned(p.name, v)`, with `set_owned` freeing its
  first argument, freed the old string twice through a `heap.new` box: once
  in `set_owned`, once in the store. `string.free(p.name)` then
  `heap.free(p)` did the same through the destructor, and so did a field of
  a struct a call returned, which the statement destroys afterwards (#2576).
  A field handed to a call that frees it now gives its reference up at the
  call (a plain `malloc`'d buffer, which such a call cannot free, stays
  with the field), and `string.free(p.name)` frees it through its tracker
  and empties the field.
- **An assignment as an unbraced match arm is a statement (#2575, #2578).**
  `1 -> o.name = s` was parsed as an expression and emitted as a bare C
  assignment: the field neither freed what it replaced nor took ownership,
  and `1 -> x = s` skipped a heap-tracked local's reassignment. The parser
  gives it the tree `1 -> { o.name = s }` has, so it is emitted as one, and
  a local stored by the arm is freed when another arm runs.
- **A struct in an actor's state that owns strings is replaced, not
  overwritten (#2579).** `kept = make_rec(w)` leaked the old value's
  strings on every message, and `kept = r`, `r` a handler local, shared
  `r`'s strings, which the handler's exit freed. And `kept.name = s`
  compiled to C naming an undeclared `kept` (#2581); it is `self->kept`.
- **The runtime free under another name is a free (#2587).**
  `@extern("string_free") drop_raw(s: string)`, as `std.jsonpath` declares
  it, was not recognised as one, so a local handed to it was freed again at
  scope exit: jsonpath did that on every malformed member name.
- **A field store trusts a pointer's trackers only when every binding of
  it is a `heap.new` box (#2580).** A local bound to `heap.new` on one
  branch and to `malloc(n) as *T` on another had its garbage tracker read
  (#1873) on the second.
- **A string cast to or from a distinct type is owned as its operand is
  (#2563).** `x as Tag` hid the expression under it from every ownership
  rule. `std.language`'s `base()` (`return language(tag) as Tag`) leaked its
  result on every call, and `t = x as Tag` borrowed a local that `t = x`
  copies, so a function returning `t` returned a buffer its own exit had
  freed. Codegen now drops a string-to-string cast and sees the operand.
- **`std.jsonpath` no longer leaks a copy of its diagnostic per filter
  operand (#2564).** A heap string local stored into a struct's string field
  on one path counted as escaping on every path, so the local was never
  freed: the parser saves `p.err`, puts it back only when a speculative
  parse fails, and leaked the saved copy whenever it succeeded. The store
  moves the local's ownership into the field; every other path frees it.
- **`heap.free` of an `@observable` box removes its observers (#2570).**
  An observer left on a freed box held its closure environment for good,
  and the next object allocated at that address inherited it: the
  `std.observe` README's own example did that on every run. Freeing an
  object any other way still needs `observe.unobserve_all` first.
- **`url.query_free` releases a parsed query (#2571).** `parse_query` hands
  back a list the caller owns, and nothing in `std.url` released it: the
  README example leaked it. A malformed query returned its partly filled
  list beside the error, so a caller that checked the error leaked that
  too; it now frees what it parsed and returns a null list, which
  `query_get`, `query_get_all` and `query_free` take as an empty query.
- **Diagnostics name a distinct type by its own name (#2567).** Passing an
  `int` where `type Age = distinct int` is expected said "expected int, got
  int"; it says "expected Age, got int".
- **An edit to a module's `@c_include` header rebuilds (#2560).** The build
  cache key did not cover the header (the C compiler reads it, not
  `aetherc`), so `ae build` and `ae run` served an object built from the old
  one. The header beside the module is now part of the key, as `@source`
  files are.
- **The Windows release build has the time its tests take (#2557).** 0.793.0
  was tagged but never published: the release job capped every platform at
  45 minutes, and the Windows test step alone takes about 36. The Windows
  leg now has 80 minutes; the others keep 45.
- **The `std.bignum` README example frees its values (#2568),** and the
  `contrib.host.tinygo` README describes the module as it is: `call_dynamic`
  exists behind libffi, 35 wrappers rather than 5, and a string result is
  borrowed, so a `C.CString` stays valid until it is freed (who should free
  it is #2569).
