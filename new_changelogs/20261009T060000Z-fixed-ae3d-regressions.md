- **A function returning a struct that holds its string parameter compiles
  again.** `cursor(text) -> Cursor { return Cursor { text: text, ... } }`
  was taken for a call that hands its string parameter back, so a local bound
  to its result was typed as the string and the C did not compile (0.792 to
  0.795). Only a call whose value is a string can be its string argument. Such
  a function now also takes its own reference to the parameter, which the
  returned struct owns, and its caller frees its own string, as for any other
  call: before, nobody freed it (#2584).
- **A call statement whose later argument is a call compiles without
  warnings.** When such a statement's earlier arguments are evaluated into
  temporaries first, the statement's "value discarded" mark went to the first
  call emitted instead of to the statement's own: a hoisted call that frees a
  string was cast to void, so the C did not compile (`'_eo0' declared void`,
  #2589), and the statement's value was left unused (clang
  `-Wunused-value`, #2585). The mark now names the statement's call.
- **`&f()` is a type error that says what to do.** A call's result is not
  stored anywhere, so it has no address; `&make_pair()` passed the type
  checker and failed in the C compiler with "lvalue required". It now asks
  for a local first (`p = make_pair()`, then `&p`) (#2591).
- **A module-level `var` of a struct type takes a struct literal of
  constants.** `var g: Pair = Pair { a: 0, b: 0 }` was refused as not a
  compile-time constant. It is emitted as an initializer list, which C takes
  for a file-scope static: nested literals, a header struct's string field (a
  plain C string) and a closure field set to `null` included (#2590).
- **`extern union Name @c_import` declares a union a C header defines.** A
  header's union had to be declared `extern struct`, and `sizeof`, a pointer
  cast or a parameter of its pointer type spelled it `struct Name`, a tag
  mismatch clang rejects. It is spelled `union Name` now. An `extern union`
  without `@c_import` is an error, since Aether does not emit a union's
  layout (#2561).
- **An edit to a header a `@c_include` header includes rebuilds.** The cache
  key held the `@c_include` header (#2560) but not the headers it includes
  with `#include "..."`, so `ae build` and `ae run` served the object built
  from the old one. Those are folded into the key now, recursively, an absent
  one by its absence; past the scan's limit the key falls back to the tree
  walk (#2577).
