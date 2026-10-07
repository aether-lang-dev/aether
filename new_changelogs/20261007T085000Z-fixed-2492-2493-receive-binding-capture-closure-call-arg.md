- **A closure in a receive arm can capture the names the arm's message
  pattern binds (#2492).** `Ping(n) -> { g = || { println("n=${n}") } }`
  failed in the C compiler with "'n' undeclared": capture discovery only
  looked at the declarations in the arm's body, and the pattern's bindings
  (`Ping(n)`, `Named(who: name)`) are declared by the handler from the
  message. They now count as the arm's own names, for int, string and `ptr`
  fields, in closures nested at any depth. A capture in an arm (a binding or
  a local) also takes its type from the arm; it was looked up through every
  function, so a string local captured from an arm was declared int. A
  closure that writes a binding gets it as a shared cell seeded from the
  message, and a string cell holds its own reference, so the message's
  string is still released once.
- **A heap-string argument to a closure call is freed after the call (#2493).**
  `call(f, mk("x"))`, and `f(mk("x"))` on a closure local, passed the
  argument straight to the closure, so every such call leaked the string,
  while a named call `g(mk("x"))` hoists it into a temporary and frees it
  after the call. A closure call now gets the same treatment, decided by the
  closure's body as a function's is: an argument the closure only reads is
  freed, one it keeps (in a captured variable, a list, a nested closure) is
  left to its new owner, and one returned as the string result is freed
  unless the result is that same pointer. A call through an `fn` parameter,
  or a variable bound to more than one closure, has no body to read and still
  leaves the argument alone.
