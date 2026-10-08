- **A handed-over closure is freed wherever it is used, a hand-off in a
  condition or a `defer` no longer keeps every env of a local, and a named
  function used as a closure value no longer leaks the string its adapter
  passes it (#2507).** A closure a function hands over (a fresh one only it
  held) is released after the call that consumes it anywhere in an
  expression (`x = take(make_counter(r))`), and at once when thrown away.
  A local that hands its value on inside an `if` condition, a loop
  condition whose body does not rebind it, a statement with a trailing
  block or a `defer` stops owning it at that point and still frees the
  closures it is bound to afterwards; a hand-off inside a nested closure
  body still keeps every env of the local. Assigning a `string` parameter
  to a local is a keep only when that local keeps the value, so neither a
  closure nor a function's closure adapter takes a reference that nothing
  gives back.
