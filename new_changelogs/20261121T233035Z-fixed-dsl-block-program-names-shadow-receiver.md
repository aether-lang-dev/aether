- **In a trailing block, a name the program binds comes before the block
  receiver's function of that name.** The receiver fallback (#333) was asked
  ahead of everything outside the block, so in `child.run(h) callback {
  say(1, 2, 3) }` a program's three-argument `say` was checked against the
  module's `say` ("expects 2 argument(s), got 3"), a program's function of
  the module's own arity built and silently called the module's, and a local
  or parameter read in the block (`println(label)`) was the module's function
  of that name. The program's own locals, parameters, closures bound to
  locals and top-level functions, types, constants and `var`s now shadow the
  receiver, in the checker, inference, the settle of what a function returns
  and codegen alike, as `docs/closures-and-builder-dsl.md` says; where the
  program binds nothing of the name the receiver's function is still what it
  means, and builtins and names an import bound still lose to it. A
  non-callable local named like the receiver's function is refused naming
  that function. `contrib/parsers/xml_expat`'s spec defined a `fail` that
  exits, which its `it` blocks had reached as std.spec's: it now has none.
