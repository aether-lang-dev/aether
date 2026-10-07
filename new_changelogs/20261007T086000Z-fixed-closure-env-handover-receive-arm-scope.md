- **A returned closure is freed by its caller, a closure captured by another
  is freed with it, and a receive arm is a scope (#2494, #2498).** Three
  closure environments were never freed, and with them the shared cells and
  strings they hold. A closure a function returned was not freed by the
  caller's local, since a call result could be shared. An environment that
  captured another closure copied it without taking a reference, so the
  captured environment leaked once its own local could not free it. A
  receive arm was emitted without a scope, so a `defer` in an arm never ran
  and nothing an arm built (closures, cells, struct strings) was released.
  Now every closure environment carries a reference count: capturing a
  closure takes a reference and the environment's destructor gives it back.
  A function whose every `return` hands back a closure nothing else holds
  (a literal, or a local whose only escape is the return) gives that
  reference to its caller, and a local bound to its result frees it at
  scope exit, under the same rules as a local bound to a closure literal.
  Each handler and timeout arm is a scope, so its defers, cell releases,
  environment frees and struct destroys run when the handler ends, on every
  exit. A struct local stored into actor state, or captured by a closure in
  the arm, keeps its strings.
