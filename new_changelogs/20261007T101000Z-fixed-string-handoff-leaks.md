- **Three string and closure references are given back: a kept closure
  string parameter, a closure local's returned closure, and a message field
  built from a temporary.** A closure that keeps a `string` parameter takes
  its own reference on entry (#2499), but when the keep was only an alias
  into a local (`nm = item`) nothing released it; the parameter is now a
  tracked string, so an alias moves the reference, a return hands it to the
  caller and whatever it still holds at exit is freed. A call through a
  closure local whose literal returns a closure only it holds
  (`held = keep(base)`) is now a fresh binding, freed at scope exit like a
  named function's (#2494); it leaked the env, its cell and the cell's
  string. A message string field built from a call or an interpolation
  (`w ! Keep { s: string.concat(p, "pt") }`, and the same in an ask or a
  reply) was copied for the receiver and the temporary was never freed; it
  is now freed once copied. The macOS leaks gate found all three.
